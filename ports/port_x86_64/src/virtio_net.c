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
 * The device is polled and asks for no interrupt at all: a frame arrives
 * when the stack asks for one, the receive ring is read on every poll, and a
 * line of the card would wake a task that is not waiting for anything. The
 * disk of the port is the other answer (src/virtio_blk.c, ADR 0037): its
 * driver waits for the interrupt of its queue because a program that reads a
 * file is a task that has nothing else to do, and the line of the card is a
 * line of the 8259 the port can open.
 *
 * The memory the device reads and writes is the memory of the kernel, and
 * the port translates it the way ADR 0032 says: the heap is the identity map
 * of the first gigabyte and the image is a fixed offset of it, so the address
 * of a ring is the address the device wants, and
 * port_x86_64_bus_phys() is the one place that says so.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port/mem.h"
#include "kernel/port/time.h"
#include "port_x86_64_net.h"
#include "port_x86_64_pci.h"
#include "port_x86_64_virtio.h"

/* --- the card on the PCI bus ---------------------------------------------- */

#define PCI_VENDOR_REDHAT 0x1AF4u
#define PCI_DEVICE_VIRTIO_NET_LEGACY 0x1000u
#define PCI_DEVICE_VIRTIO_NET_MODERN 0x1041u

/* --- what a network card has that a block device has not ------------------ */

/* The one feature the driver takes: the device has a MAC for it. */
#define VIRTIO_NET_F_MAC (1u << 5)

/* The queues of a network device, in the order of the specification. */
#define VIRTIO_NET_QUEUE_RX 0u
#define VIRTIO_NET_QUEUE_TX 1u

/* The header of a legacy virtio-net packet: ten bytes of an offload the
 * driver never asks for, and a payload after it. VIRTIO_F_VERSION_1 would
 * make it twelve, and the driver does not negotiate that feature. */
#define VIRTIO_NET_HDR_LEGACY 10u

/* A buffer of the receive queue holds the header and the largest frame. */
#define VIRTIO_NET_RX_BUFFER 2048u

/* The transmit path copies the frame after the header into this buffer. */
#define VIRTIO_NET_TX_BUFFER 2048u

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

/* --- the init of the device ----------------------------------------------- */

static bool vio_config_mac(k_u8_t mac[6])
{
    k_size_t i;

    for (i = 0; i < 6u; i++) {
        mac[i] = virtio_in8(g_vio_base, (k_u16_t)(VIRTIO_PCI_CONFIG + i));
    }

    return true;
}

static void vio_status(k_u8_t status)
{
    virtio_status(g_vio_base, status);
}

static void vio_queue_notify(k_u16_t queue)
{
    virtio_notify(g_vio_base, queue);
}

/*
 * Program one queue: the shared call selects it, learns its size, builds the
 * ring and gives the device the page number. The transmit queue is set up
 * empty; the receive queue is filled with buffers before the driver says the
 * device may run, because a receive that arrives into a queue with no buffer
 * is a frame the device drops.
 *
 * The rings of this driver are polled: the line of the card stays masked and
 * the device is asked for no interrupt at all.
 */
static bool vio_queue_setup(struct virtqueue *queue, k_u16_t index)
{
    if (!virtio_queue_setup(g_vio_base, queue, index, false)) {
        kerror("virtio0: the queue %u of the card is not usable", (unsigned)index);
        return false;
    }

    return true;
}

static bool vio_rx_fill(void)
{
    k_u16_t i;
    k_u64_t base = port_x86_64_bus_phys(g_rx_buffers);

    for (i = 0; i < g_rx_queue.size; i++) {
        virtqueue_descriptor(&g_rx_queue, i, base + (k_u64_t)i * VIRTIO_NET_RX_BUFFER,
                             VIRTIO_NET_RX_BUFFER, VIRTQ_DESC_F_WRITE, 0);
        virtqueue_avail_push(&g_rx_queue, i);
    }

    virtio_barrier();
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
    struct port_x86_64_pci_device card;
    k_u32_t bar0;

    /*
     * A machine with a modern-only card is named instead of silently left
     * without a network: the driver speaks the legacy window, and a card
     * that has none would answer "no" for a reason nobody could see.
     */
    if (port_x86_64_pci_find(PCI_VENDOR_REDHAT, PCI_DEVICE_VIRTIO_NET_MODERN, NULL)) {
        kerror("virtio0: the machine offers only the modern virtio "
               "interface, which this driver does not speak");
        return false;
    }

    if (!port_x86_64_pci_find(PCI_VENDOR_REDHAT, PCI_DEVICE_VIRTIO_NET_LEGACY, &card)) {
        return false;
    }

    bar0 = port_x86_64_pci_read32(&card, PCI_BAR0);

    if ((bar0 & 1u) == 0u) {
        kerror("virtio0: the legacy BAR is not an I/O window");
        return false;
    }

    g_vio_base = (k_u16_t)(bar0 & ~3u);

    /* The window and the bus are the two switches of the card: without the
     * first the registers read as nothing, without the second the device is
     * not allowed to use the memory of the rings. */
    port_x86_64_pci_enable(&card, PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    return true;
}

static bool vio_start(void)
{
    k_u32_t features;
    k_u32_t wanted;

    if (!virtio_reset(g_vio_base)) {
        kerror("virtio0: the device did not take a reset");
        return false;
    }

    vio_status(VIRTIO_STATUS_ACKNOWLEDGE);
    vio_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    features = virtio_in32(g_vio_base, VIRTIO_PCI_DEVICE_FEATURES);
    wanted = features & VIRTIO_NET_F_MAC;

    if ((wanted & VIRTIO_NET_F_MAC) == 0u) {
        kerror("virtio0: the device has no MAC feature");
        vio_status(VIRTIO_STATUS_FAILED);
        return false;
    }

    virtio_out32(g_vio_base, VIRTIO_PCI_DRIVER_FEATURES, wanted);

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

    virtqueue_descriptor(&g_tx_queue, 0, g_tx_buffer_phys, VIRTIO_NET_HDR_LEGACY, 0, 0);

    if (!vio_rx_fill()) {
        vio_status(VIRTIO_STATUS_FAILED);
        return false;
    }

    vio_config_mac(g_vio_mac);

    virtio_barrier();
    vio_status(VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    g_vio_link = (virtio_in16(g_vio_base, VIRTIO_PCI_CONFIG + 6u) & 1u) != 0u;

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

    g_vio_link = (virtio_in16(g_vio_base, VIRTIO_PCI_CONFIG + 6u) & 1u) != 0u;

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
    (void)virtio_in8(g_vio_base, VIRTIO_PCI_ISR);
    virtio_barrier();

    used = virtqueue_used_index(&g_rx_queue);

    if (used == g_rx_queue.last_used) {
        return false;
    }

    virtqueue_used_entry(&g_rx_queue, g_rx_queue.last_used, &id, &bytes);
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
    virtqueue_avail_push(&g_rx_queue, (k_u16_t)id);
    virtio_barrier();
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

    virtqueue_descriptor(&g_tx_queue, 0, g_tx_buffer_phys, (k_u32_t)(length + VIRTIO_NET_HDR_LEGACY), 0,
                  0);

    used_before = virtqueue_used_index(&g_tx_queue);

    virtqueue_avail_push(&g_tx_queue, 0);
    virtio_barrier();
    vio_queue_notify(VIRTIO_NET_QUEUE_TX);

    deadline = k_port_uptime_ms() + 100u;

    while (virtqueue_used_index(&g_tx_queue) == used_before) {
        if (k_port_uptime_ms() > deadline) {
            return false;
        }

        /* The wait is short and a frame of a request is a few hundred
         * bytes; a pause is enough and leaves the timer to the other
         * tasks. */
        __asm__ volatile("pause");
    }

    virtqueue_used_entry(&g_tx_queue, used_before, &id, &bytes);

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
