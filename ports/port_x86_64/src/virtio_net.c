/*
 * Micro OS — the x86-64 port: an ethernet device on the PCI bus.
 *
 * The machine of this port had no network until K6: the contract answered
 * "no" and the commands that needed a socket said so. This file is the
 * first device of the port behind the PCI bus, and it speaks virtio-net in
 * its legacy interface (the I/O BAR, a split virtqueue, no features beyond
 * the MAC address).
 *
 * virtio and not e1000, for two reasons that are about this machine and not
 * about taste. The legacy virtio device is programmed through the port I/O
 * space, so a frame path needs no mapping: the kernel of the port is the
 * identity map of the first gigabyte, and the MMIO BAR of a PCI device in
 * QEMU sits near 4 GiB, outside the map. The e1000 keeps its rings behind
 * exactly that BAR, so it would need a mapping of MMIO in every address
 * space of the machine before it could send its first frame - the paging
 * work of ADR 0007, not a driver. And the virtqueue is a public format with
 * a document, while a good half of the e1000 is a register file whose
 * quirks are learned from the driver of another system.
 *
 * The device is polled. The timer and the keyboard are the only lines of the
 * PIC this port unmasks, and a PCI interrupt needs the routing of the line
 * through the I/O APIC (or MSI), which is K8. Polling the used ring of the
 * receive queue and waiting for the used entry of the one transmit
 * descriptor is what a kernel with one task on the network needs, and it
 * keeps the driver free of the interrupt controller the way `ata.c` is.
 *
 * The memory the device reads and writes is the memory of the kernel: the
 * port has no physical address contract yet, but its kernel runs on an
 * identity map, so the address of a buffer is the address the device wants.
 * The day the kernel moves to a higher half (ADR 0007) this file asks the
 * paging code for the translation, and the queues are the only place it
 * does.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port/mem.h"
#include "kernel/port/time.h"
#include "port_x86_64_net.h"

/* --- the configuration space of the PCI bus ------------------------------- */

/*
 * Mechanism 1, the one every PC has had since the 440FX: a 32-bit address
 * written to 0xCF8 and the answer read from 0xCFC. The bus of the machine is
 * flat in QEMU, and a device under a bridge is a scan this driver does not
 * need yet.
 */
#define PCI_CONFIG_ADDRESS 0xCF8u
#define PCI_CONFIG_DATA 0xCFC

#define PCI_VENDOR 0x00u
#define PCI_DEVICE 0x02u
#define PCI_COMMAND 0x04u
#define PCI_BAR0 0x10u
#define PCI_HEADER_TYPE 0x0Eu

#define PCI_COMMAND_IO 0x0001u
#define PCI_COMMAND_MEMORY 0x0002u
#define PCI_COMMAND_BUS_MASTER 0x0004u

#define PCI_VENDOR_REDHAT 0x1AF4u
#define PCI_DEVICE_VIRTIO_NET_LEGACY 0x1000u
#define PCI_DEVICE_VIRTIO_NET_MODERN 0x1041u

static k_u32_t pci_read32(k_u8_t bus, k_u8_t slot, k_u8_t function, k_u8_t offset)
{
    k_u32_t address = 0x80000000u | ((k_u32_t)bus << 16) | ((k_u32_t)slot << 11) |
                      ((k_u32_t)function << 8) | (offset & 0xFCu);

    port_outl(PCI_CONFIG_ADDRESS, address);

    return port_inl(PCI_CONFIG_DATA);
}

static void pci_write32(k_u8_t bus, k_u8_t slot, k_u8_t function, k_u8_t offset, k_u32_t value)
{
    k_u32_t address = 0x80000000u | ((k_u32_t)bus << 16) | ((k_u32_t)slot << 11) |
                      ((k_u32_t)function << 8) | (offset & 0xFCu);

    port_outl(PCI_CONFIG_ADDRESS, address);
    port_outl(PCI_CONFIG_DATA, value);
}

static k_u16_t pci_read16(k_u8_t bus, k_u8_t slot, k_u8_t function, k_u8_t offset)
{
    k_u32_t value = pci_read32(bus, slot, function, offset & 0xFCu);

    return (k_u16_t)((value >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

static void pci_write16(k_u8_t bus, k_u8_t slot, k_u8_t function, k_u8_t offset, k_u16_t value)
{
    k_u32_t old = pci_read32(bus, slot, function, offset & 0xFCu);
    k_u32_t shift = (offset & 2u) * 8u;
    k_u32_t mask = 0xFFFFu << shift;

    pci_write32(bus, slot, function, offset & 0xFCu,
                (old & ~mask) | (((k_u32_t)value << shift) & mask));
}

/* --- the legacy interface of virtio --------------------------------------- */

/*
 * The register file of a legacy virtio device in its I/O BAR. The modern
 * interface of virtio 1.0 replaces it with a capability in the config
 * space; this driver asks QEMU for a transitional device, whose legacy
 * window is always there, and says so when the machine offers only the
 * modern one.
 */
#define VIRTIO_PCI_DEVICE_FEATURES 0x00u
#define VIRTIO_PCI_DRIVER_FEATURES 0x04u
#define VIRTIO_PCI_QUEUE_ADDRESS 0x08u
#define VIRTIO_PCI_QUEUE_SIZE 0x0Cu
#define VIRTIO_PCI_QUEUE_SELECT 0x0Eu
#define VIRTIO_PCI_QUEUE_NOTIFY 0x10u
#define VIRTIO_PCI_STATUS 0x12u
#define VIRTIO_PCI_ISR 0x13u
#define VIRTIO_PCI_CONFIG 0x14u

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01u
#define VIRTIO_STATUS_DRIVER 0x02u
#define VIRTIO_STATUS_DRIVER_OK 0x04u
#define VIRTIO_STATUS_FAILED 0x80u

/* The one feature the driver takes: the device has a MAC for it. */
#define VIRTIO_NET_F_MAC (1u << 5)

/* The queues of a network device, in the order of the specification. */
#define VIRTIO_NET_QUEUE_RX 0u
#define VIRTIO_NET_QUEUE_TX 1u

/* The descriptor: an address the device reads or writes, a length and two
 * flags. VIRTQ_DESC_F_WRITE says the device writes the buffer (a receive). */
#define VIRTQ_DESC_F_NEXT 0x0001u
#define VIRTQ_DESC_F_WRITE 0x0002u

/* The driver takes the interrupts of the device off and polls the rings. */
#define VIRTQ_AVAIL_F_NO_INTERRUPT 0x0001u
#define VIRTQ_USED_F_NO_NOTIFY 0x0001u

/* The header of a legacy virtio-net packet: ten bytes of an offload the
 * driver never asks for, and a payload after it. VIRTIO_F_VERSION_1 would
 * make it twelve, and the driver does not negotiate that feature. */
#define VIRTIO_NET_HDR_LEGACY 10u

/* A buffer of the receive queue holds the header and the largest frame. */
#define VIRTIO_NET_RX_BUFFER 2048u

/* The transmit path copies the frame after the header into this buffer. */
#define VIRTIO_NET_TX_BUFFER 2048u

/* A queue of the device: the split ring and the point the driver reached. */
struct virtqueue {
    k_u16_t size;
    k_u8_t *desc;      /* size entries of sixteen bytes */
    k_u8_t *avail;     /* flags, idx, ring[size], used_event */
    k_u8_t *used;      /* flags, idx, ring[size] of {id, len}, avail_event */
    k_u16_t last_used; /* the used entry the driver has not looked at yet */
    k_u8_t *memory;    /* the allocation behind the ring, page aligned */
};

static k_u16_t g_vio_base; /* the first port of the I/O BAR */
static k_u8_t g_vio_mac[6];
static bool g_vio_present;
static bool g_vio_link;

static struct virtqueue g_rx_queue;
static struct virtqueue g_tx_queue;

static k_u8_t *g_rx_buffers;
static k_u64_t g_rx_buffers_phys;
static k_u8_t *g_tx_buffer;
static k_u64_t g_tx_buffer_phys;

static k_u64_t g_rx_frames;
static k_u64_t g_tx_frames;
static k_u64_t g_rx_dropped;

static k_u16_t vio_in16(k_u16_t offset)
{
    return port_inw((k_u16_t)(g_vio_base + offset));
}

static void vio_out16(k_u16_t offset, k_u16_t value)
{
    port_outw((k_u16_t)(g_vio_base + offset), value);
}

static k_u8_t vio_in8(k_u16_t offset)
{
    return port_inb((k_u16_t)(g_vio_base + offset));
}

static void vio_out8(k_u16_t offset, k_u8_t value)
{
    port_outb((k_u16_t)(g_vio_base + offset), value);
}

static k_u32_t vio_in32(k_u16_t offset)
{
    return port_inl((k_u16_t)(g_vio_base + offset));
}

static void vio_out32(k_u16_t offset, k_u32_t value)
{
    port_outl((k_u16_t)(g_vio_base + offset), value);
}

/* The stores of the driver must be in the memory before the device is told
 * to look at them; the processor of this machine does not reorder them, but
 * the compiler may, and this is the line that stops it. */
static void vio_barrier(void)
{
    __asm__ volatile("" : : : "memory");
}

/* --- the rings ------------------------------------------------------------ */

/*
 * One queue: a region of pages the device finds from its number, and the
 * layout the legacy interface fixes. The descriptor table starts at the
 * base, the available ring follows it, and the used ring starts at the next
 * page boundary (the alignment QEMU uses, VIRTIO_PCI_VRING_ALIGN). The
 * queue size is the device's and not the driver's: in the legacy interface
 * it is read-only.
 */
static k_u64_t vq_layout_used_offset(k_u16_t size)
{
    k_u64_t after_avail = (k_u64_t)size * 16u + 6u + (k_u64_t)size * 2u;

    return (after_avail + 4095u) & ~(k_u64_t)4095u;
}

static bool vq_allocate(struct virtqueue *queue, k_u16_t size)
{
    k_u64_t used_offset = vq_layout_used_offset(size);
    k_size_t total = (k_size_t)(used_offset + 6u + (k_u64_t)size * 8u);
    k_u8_t *raw = (k_u8_t *)k_port_alloc(total + 4096u, false);
    k_u8_t *aligned;

    if (raw == NULL) {
        return false;
    }

    aligned = (k_u8_t *)(((k_u64_t)(uintptr_t)raw + 4095u) & ~(k_u64_t)4095u);

    memset(aligned, 0, total);

    queue->size = size;
    queue->memory = raw;
    queue->desc = aligned;
    queue->avail = aligned + (k_size_t)size * 16u;
    queue->used = aligned + (k_size_t)used_offset;
    queue->last_used = 0;

    return true;
}

static void vq_descriptor(struct virtqueue *queue, k_u16_t index, k_u64_t address, k_u32_t length,
                          k_u16_t flags, k_u16_t next)
{
    k_u8_t *desc = queue->desc + (k_size_t)index * 16u;

    desc[0] = (k_u8_t)(address & 0xFFu);
    desc[1] = (k_u8_t)((address >> 8) & 0xFFu);
    desc[2] = (k_u8_t)((address >> 16) & 0xFFu);
    desc[3] = (k_u8_t)((address >> 24) & 0xFFu);
    desc[4] = (k_u8_t)((address >> 32) & 0xFFu);
    desc[5] = (k_u8_t)((address >> 40) & 0xFFu);
    desc[6] = (k_u8_t)((address >> 48) & 0xFFu);
    desc[7] = (k_u8_t)((address >> 56) & 0xFFu);

    desc[8] = (k_u8_t)(length & 0xFFu);
    desc[9] = (k_u8_t)((length >> 8) & 0xFFu);
    desc[10] = (k_u8_t)((length >> 16) & 0xFFu);
    desc[11] = (k_u8_t)((length >> 24) & 0xFFu);

    desc[12] = (k_u8_t)(flags & 0xFFu);
    desc[13] = (k_u8_t)((flags >> 8) & 0xFFu);
    desc[14] = (k_u8_t)(next & 0xFFu);
    desc[15] = (k_u8_t)((next >> 8) & 0xFFu);
}

static k_u16_t vq_avail_index(struct virtqueue *queue)
{
    return (k_u16_t)(queue->avail[2] | ((k_u16_t)queue->avail[3] << 8));
}

static void vq_avail_set_index(struct virtqueue *queue, k_u16_t value)
{
    queue->avail[2] = (k_u8_t)(value & 0xFFu);
    queue->avail[3] = (k_u8_t)(value >> 8);
}

static void vq_avail_push(struct virtqueue *queue, k_u16_t head)
{
    k_u16_t index = vq_avail_index(queue);
    k_u8_t *slot = queue->avail + 4u + (k_size_t)(index % queue->size) * 2u;

    slot[0] = (k_u8_t)(head & 0xFFu);
    slot[1] = (k_u8_t)(head >> 8);

    vio_barrier();
    vq_avail_set_index(queue, (k_u16_t)(index + 1u));
}

static k_u16_t vq_used_index(struct virtqueue *queue)
{
    return (k_u16_t)(queue->used[2] | ((k_u16_t)queue->used[3] << 8));
}

static void vq_used_entry(struct virtqueue *queue, k_u16_t index, k_u32_t *id, k_u32_t *length)
{
    k_u8_t *entry = queue->used + 4u + (k_size_t)(index % queue->size) * 8u;

    *id = (k_u32_t)entry[0] | ((k_u32_t)entry[1] << 8) | ((k_u32_t)entry[2] << 16) |
          ((k_u32_t)entry[3] << 24);
    *length = (k_u32_t)entry[4] | ((k_u32_t)entry[5] << 8) | ((k_u32_t)entry[6] << 16) |
              ((k_u32_t)entry[7] << 24);
}

/* --- the init of the device ----------------------------------------------- */

static bool vio_config_mac(k_u8_t mac[6])
{
    k_size_t i;

    for (i = 0; i < 6u; i++) {
        mac[i] = vio_in8((k_u16_t)(VIRTIO_PCI_CONFIG + i));
    }

    return true;
}

static void vio_status(k_u8_t status)
{
    vio_out8(VIRTIO_PCI_STATUS, status);
}

static void vio_queue_notify(k_u16_t queue)
{
    vio_out16(VIRTIO_PCI_QUEUE_NOTIFY, queue);
}

/*
 * Program one queue: select it, learn its size, build the ring and give the
 * device the page number. The transmit queue is set up empty; the receive
 * queue is filled with buffers before the driver says the device may run,
 * because a receive that arrives into a queue with no buffer is a frame the
 * device drops.
 */
static bool vio_queue_setup(struct virtqueue *queue, k_u16_t index)
{
    k_u16_t size;
    k_u64_t phys;

    vio_out16(VIRTIO_PCI_QUEUE_SELECT, index);
    size = vio_in16(VIRTIO_PCI_QUEUE_SIZE);

    if (size < 2u || size > 1024u) {
        kerror("virtio0: the queue %u has an impossible size %u", (unsigned)index, (unsigned)size);
        return false;
    }

    if (!vq_allocate(queue, size)) {
        kerror("virtio0: no memory for the queue %u of %u entries", (unsigned)index,
               (unsigned)size);
        return false;
    }

    /* The driver never wants an interrupt from this device: the rings are
     * polled and the line of the card stays masked. */
    queue->avail[0] = (k_u8_t)VIRTQ_AVAIL_F_NO_INTERRUPT;
    queue->avail[1] = 0;
    queue->used[0] = (k_u8_t)VIRTQ_USED_F_NO_NOTIFY;
    queue->used[1] = 0;

    phys = port_x86_64_bus_phys(queue->desc);

    vio_barrier();
    vio_out32(VIRTIO_PCI_QUEUE_ADDRESS, (k_u32_t)(phys >> 12));

    return true;
}

static bool vio_rx_fill(void)
{
    k_u16_t i;
    k_u64_t base = port_x86_64_bus_phys(g_rx_buffers);

    for (i = 0; i < g_rx_queue.size; i++) {
        vq_descriptor(&g_rx_queue, i, base + (k_u64_t)i * VIRTIO_NET_RX_BUFFER,
                      VIRTIO_NET_RX_BUFFER, VIRTQ_DESC_F_WRITE, 0);
        vq_avail_push(&g_rx_queue, i);
    }

    vio_barrier();
    vio_queue_notify(VIRTIO_NET_QUEUE_RX);

    return true;
}

/*
 * Find the device and bring it up. The device is the ethernet card of the
 * machine; a machine without one (the default of make x86-run) leaves the
 * stack with no frames and the contract with its "no" answers, which is a
 * working network of zero devices and not an error of the boot.
 */
static bool vio_probe(void)
{
    k_u8_t bus;
    k_u8_t slot;
    k_u8_t function;

    for (bus = 0; bus < 1u; bus++) {
        for (slot = 0; slot < 32u; slot++) {
            for (function = 0; function < 8u; function++) {
                k_u16_t vendor = pci_read16(bus, slot, function, PCI_VENDOR);
                k_u16_t device;
                k_u32_t bar0;
                k_u16_t command;

                if (vendor != PCI_VENDOR_REDHAT) {
                    continue;
                }

                device = pci_read16(bus, slot, function, PCI_DEVICE);

                if (device == PCI_DEVICE_VIRTIO_NET_MODERN) {
                    kerror("virtio0: the machine offers only the modern virtio "
                           "interface, which this driver does not speak");
                    return false;
                }

                if (device != PCI_DEVICE_VIRTIO_NET_LEGACY) {
                    continue;
                }

                bar0 = pci_read32(bus, slot, function, PCI_BAR0);

                if ((bar0 & 1u) == 0u) {
                    kerror("virtio0: the legacy BAR is not an I/O window");
                    return false;
                }

                g_vio_base = (k_u16_t)(bar0 & ~3u);

                /* The window and the bus are the two switches of the card:
                 * without the first the registers read as nothing, without
                 * the second the device is not allowed to use the memory of
                 * the rings. */
                command = pci_read16(bus, slot, function, PCI_COMMAND);
                command |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
                pci_write16(bus, slot, function, PCI_COMMAND, command);

                return true;
            }
        }
    }

    return false;
}

static bool vio_start(void)
{
    k_u32_t features;
    k_u32_t wanted;

    /* A reset is a zero in the status register and the device may take a
     * moment to honour it; the loop is the legacy handshake. */
    vio_status(0);

    {
        k_u64_t deadline = k_port_uptime_ms() + 100u;

        while (vio_in8(VIRTIO_PCI_STATUS) != 0u) {
            if (k_port_uptime_ms() > deadline) {
                kerror("virtio0: the device did not take a reset");
                return false;
            }
        }
    }

    vio_status(VIRTIO_STATUS_ACKNOWLEDGE);
    vio_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    features = vio_in32(VIRTIO_PCI_DEVICE_FEATURES);
    wanted = features & VIRTIO_NET_F_MAC;

    if ((wanted & VIRTIO_NET_F_MAC) == 0u) {
        kerror("virtio0: the device has no MAC feature");
        vio_status(VIRTIO_STATUS_FAILED);
        return false;
    }

    vio_out32(VIRTIO_PCI_DRIVER_FEATURES, wanted);

    if (!vio_queue_setup(&g_rx_queue, VIRTIO_NET_QUEUE_RX)) {
        vio_status(VIRTIO_STATUS_FAILED);
        return false;
    }

    if (!vio_queue_setup(&g_tx_queue, VIRTIO_NET_QUEUE_TX)) {
        vio_status(VIRTIO_STATUS_FAILED);
        return false;
    }

    g_rx_buffers = (k_u8_t *)k_port_alloc((k_size_t)g_rx_queue.size * VIRTIO_NET_RX_BUFFER, false);

    if (g_rx_buffers == NULL) {
        kerror("virtio0: no memory for the receive buffers");
        vio_status(VIRTIO_STATUS_FAILED);
        return false;
    }

    memset(g_rx_buffers, 0, (k_size_t)g_rx_queue.size * VIRTIO_NET_RX_BUFFER);
    g_rx_buffers_phys = port_x86_64_bus_phys(g_rx_buffers);

    g_tx_buffer = (k_u8_t *)k_port_alloc(VIRTIO_NET_TX_BUFFER, false);

    if (g_tx_buffer == NULL) {
        kerror("virtio0: no memory for the transmit buffer");
        vio_status(VIRTIO_STATUS_FAILED);
        return false;
    }

    memset(g_tx_buffer, 0, VIRTIO_NET_TX_BUFFER);
    g_tx_buffer_phys = port_x86_64_bus_phys(g_tx_buffer);

    vq_descriptor(&g_tx_queue, 0, g_tx_buffer_phys, VIRTIO_NET_HDR_LEGACY, 0, 0);

    if (!vio_rx_fill()) {
        vio_status(VIRTIO_STATUS_FAILED);
        return false;
    }

    vio_config_mac(g_vio_mac);

    vio_barrier();
    vio_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_vio_link = (vio_in16(VIRTIO_PCI_CONFIG + 6u) & 1u) != 0u;

    return true;
}

void port_x86_64_net_init(void)
{
    if (!vio_probe()) {
        /* No card: the boot goes on. The stack asks this file for a frame
         * and gets "no", which is the network of a machine without one. */
        return;
    }

    if (!vio_start()) {
        return;
    }

    g_vio_present = true;

    kinfo("virtio0: %02x:%02x:%02x:%02x:%02x:%02x, %s", (unsigned)g_vio_mac[0],
          (unsigned)g_vio_mac[1], (unsigned)g_vio_mac[2], (unsigned)g_vio_mac[3],
          (unsigned)g_vio_mac[4], (unsigned)g_vio_mac[5], g_vio_link ? "link up" : "no link");
}

/* --- the frames ----------------------------------------------------------- */

const char *port_x86_64_nic_name(void)
{
    return g_vio_present ? "virtio0" : NULL;
}

bool port_x86_64_nic_present(void)
{
    return g_vio_present;
}

bool port_x86_64_nic_mac(k_u8_t mac[6])
{
    if (!g_vio_present || mac == NULL) {
        return false;
    }

    memcpy(mac, g_vio_mac, sizeof(g_vio_mac));

    return true;
}

bool port_x86_64_nic_link(void)
{
    if (!g_vio_present) {
        return false;
    }

    g_vio_link = (vio_in16(VIRTIO_PCI_CONFIG + 6u) & 1u) != 0u;

    return g_vio_link;
}

/*
 * One frame from the receive queue. The used entry names the descriptor the
 * device filled and its length, which includes the ten-byte header; the
 * frame starts after it. The descriptor is put back into the available ring
 * at once, so the device has a buffer for the next packet in the same visit.
 */
bool port_x86_64_nic_receive(void *buf, k_size_t size, k_size_t *length)
{
    k_u16_t used;
    k_u32_t id;
    k_u32_t bytes;
    k_size_t frame_len;

    if (!g_vio_present || buf == NULL) {
        return false;
    }

    /* Reading the interrupt status clears it; the line is masked and the
     * device only writes the bit. */
    (void)vio_in8(VIRTIO_PCI_ISR);
    vio_barrier();

    used = vq_used_index(&g_rx_queue);

    if (used == g_rx_queue.last_used) {
        return false;
    }

    vq_used_entry(&g_rx_queue, g_rx_queue.last_used, &id, &bytes);
    g_rx_queue.last_used++;

    if (id >= g_rx_queue.size || bytes <= VIRTIO_NET_HDR_LEGACY) {
        g_rx_dropped++;
        return false;
    }

    frame_len = (k_size_t)(bytes - VIRTIO_NET_HDR_LEGACY);

    if (frame_len > size) {
        g_rx_dropped++;
    } else {
        memcpy(buf, g_rx_buffers + (k_size_t)id * VIRTIO_NET_RX_BUFFER + VIRTIO_NET_HDR_LEGACY,
               frame_len);
        *length = frame_len;
        g_rx_frames++;
    }

    /* The buffer goes back into the ring whether its frame fit or not: a
     * dropped frame costs one packet, a buffer lost costs the queue. */
    vq_avail_push(&g_rx_queue, (k_u16_t)id);
    vio_barrier();
    vio_queue_notify(VIRTIO_NET_QUEUE_RX);

    return frame_len <= size;
}

/*
 * Give one frame to the device. The transmit queue has one descriptor: the
 * driver copies the packet behind the header, puts the descriptor into the
 * available ring and waits for the used entry. A device that does not take
 * it in the budget is a device that is gone, and the frame is reported as
 * lost instead of holding a task for ever.
 */
bool port_x86_64_nic_send(const void *frame, k_size_t length)
{
    k_u16_t used_before;
    k_u32_t id;
    k_u32_t bytes;
    k_u64_t deadline;

    if (!g_vio_present || frame == NULL || length == 0 ||
        length + VIRTIO_NET_HDR_LEGACY > VIRTIO_NET_TX_BUFFER) {
        return false;
    }

    memset(g_tx_buffer, 0, VIRTIO_NET_HDR_LEGACY);
    memcpy(g_tx_buffer + VIRTIO_NET_HDR_LEGACY, frame, length);

    vq_descriptor(&g_tx_queue, 0, g_tx_buffer_phys, (k_u32_t)(length + VIRTIO_NET_HDR_LEGACY), 0,
                  0);

    used_before = vq_used_index(&g_tx_queue);

    vq_avail_push(&g_tx_queue, 0);
    vio_barrier();
    vio_queue_notify(VIRTIO_NET_QUEUE_TX);

    deadline = k_port_uptime_ms() + 100u;

    while (vq_used_index(&g_tx_queue) == used_before) {
        if (k_port_uptime_ms() > deadline) {
            return false;
        }

        /* The wait is short and a frame of a request is a few hundred
         * bytes; a pause is enough and leaves the timer to the other
         * tasks. */
        __asm__ volatile("pause");
    }

    vq_used_entry(&g_tx_queue, used_before, &id, &bytes);

    (void)id;
    (void)bytes;

    g_tx_frames++;

    return true;
}

k_u64_t port_x86_64_nic_rx_frames(void)
{
    return g_rx_frames;
}

k_u64_t port_x86_64_nic_tx_frames(void)
{
    return g_tx_frames;
}

k_u64_t port_x86_64_nic_rx_dropped(void)
{
    return g_rx_dropped;
}
