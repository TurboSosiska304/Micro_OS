/*
 * Micro OS — the x86-64 port: an ethernet of the laptop on the PCI bus.
 *
 * The card is the Intel I219 of the machines this port is for (ADR 0055), and
 * QEMU models the family with `-device e1000e`. The driver speaks the plain
 * part of it: a control register, two rings of sixteen-byte legacy
 * descriptors, the MAC out of the NVM, and the link out of the status
 * register. There is no interrupt (the stack of src/net.c polls the card the
 * way it polls virtio and the wire of the USB), no advanced descriptor, no
 * checksum offload and no wake-on-LAN: a frame in, a frame out.
 *
 * The rings are legacy descriptors on purpose. The card of this family has a
 * second, extended format with a receive status of its own, and the extended
 * one is what the driver of Linux uses; but the legacy format is the one the
 * 82540 of the bus was designed around, the device answers it with RFCTL
 * (EXTEN clear) at reset, and a stack of one segment in flight has nothing to
 * gain from a descriptor that carries a checksum it will not use.
 *
 * The memory of the rings is the memory of the kernel: the heap of the port
 * is the identity map of the first gigabyte (ADR 0032 §1), so the address of
 * a descriptor is the address the device wants and port_x86_64_bus_phys() is
 * the one place that says so. The window of the registers is behind the BAR
 * and sits above that map, so it is mapped into the top 2 GiB of the kernel
 * before the first read, as the SATA controller and the NVMe disk are.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port/mem.h"
#include "kernel/port/time.h"
#include "port_x86_64_e1000e.h"
#include "port_x86_64_net.h"
#include "port_x86_64_pci.h"

/* --- the card on the PCI bus ---------------------------------------------- */

#define E1000_VENDOR_INTEL 0x8086u

/*
 * The functions of the family. A machine of a person has one of the I219s; a
 * machine of QEMU has the 82574L. The list is a yes/no answer and not a
 * driver per chip: the registers these ids name are the same, and a card that
 * needs a table of its own gets a line here when somebody has the machine to
 * test it on.
 */
static const k_u16_t g_e1000_ids[] = {
    0x10D3u, /* 82574L, the card of QEMU                                   */
    0x150Cu, /* 82583V                                                     */
    0x153Au, /* I217-LM                                                    */
    0x153Bu, /* I217-V                                                     */
    0x1559u, /* I218-V                                                     */
    0x155Au, /* I218-LM                                                    */
    0x156Fu, /* I219-LM                                                    */
    0x1570u, /* I219-V                                                     */
    0x15B8u, /* I219-V (2)                                                 */
    0x15D7u, /* I219-LM (3)                                                */
    0x15D8u, /* I219-V (3)                                                 */
    0x15E3u, /* I219-LM (4)                                                */
};

/* --- the registers -------------------------------------------------------- */

#define E1000_CTRL 0x0000u
#define E1000_STATUS 0x0008u
#define E1000_EERD 0x0014u
#define E1000_ICR 0x00C0u
#define E1000_IMS 0x00D0u
#define E1000_IMC 0x00D8u
#define E1000_RCTL 0x0100u
#define E1000_TCTL 0x0400u
#define E1000_TIPG 0x0410u
#define E1000_RDBAL 0x2800u
#define E1000_RDBAH 0x2804u
#define E1000_RDLEN 0x2808u
#define E1000_RDH 0x2810u
#define E1000_RDT 0x2818u
#define E1000_RXDCTL 0x2828u
#define E1000_TDBAL 0x3800u
#define E1000_TDBAH 0x3804u
#define E1000_TDLEN 0x3808u
#define E1000_TDH 0x3810u
#define E1000_TDT 0x3818u
#define E1000_TXDCTL 0x3828u
#define E1000_RAL0 0x5400u
#define E1000_RAH0 0x5404u

/* The control register: the software reset, and the bit that forces the link
 * up instead of letting the card negotiate it. */
#define E1000_CTRL_SLU 0x00000040u
#define E1000_CTRL_RST 0x04000000u

#define E1000_STATUS_LU 0x00000002u

/* The NVM read: the address goes out, the done bit comes back, the word is in
 * the upper half of the register. The fields of this register are the ones of
 * the e1000e and not the ones of the old 82540: a driver that copies the
 * numbers of the other family waits for a bit this card never sets. */
#define E1000_EERD_START 0x00000001u
#define E1000_EERD_DONE 0x00000002u
#define E1000_EERD_SHIFT 2u

/* The receive control: the card is on, broadcast frames are taken, the CRC is
 * stripped before the driver sees the frame, and a buffer is 2048 bytes. */
#define E1000_RCTL_EN 0x00000002u
#define E1000_RCTL_UPE 0x00000008u
#define E1000_RCTL_MPE 0x00000010u
#define E1000_RCTL_BAM 0x00008000u
#define E1000_RCTL_BSIZE_2048 0x00000000u
#define E1000_RCTL_SECRC 0x04000000u

/* The transmit control: on, pad short frames, and the inter-frame gap of the
 * family (10, 8 and 6 in units of the clock of the card). */
#define E1000_TCTL_EN 0x00000002u
#define E1000_TCTL_PSP 0x00000008u
#define E1000_TCTL_CT 0x00000100u
#define E1000_TCTL_COLD 0x00040000u
#define E1000_TCTL_RTLC 0x01000000u
#define E1000_TIPG_VALUE 0x0060200Au

/* The address of the card in the ring: the first one is valid, and the upper
 * half of the filter is the upper half of the MAC. */
#define E1000_RAH_AV 0x80000000u

/* The descriptor of the legacy format, and the bits of its two statuses. */
#define E1000_TXD_CMD_EOP 0x01u
#define E1000_TXD_CMD_IFCS 0x02u
#define E1000_TXD_CMD_RS 0x08u
#define E1000_TXD_STAT_DD 0x01u
#define E1000_RXD_STAT_DD 0x01u
#define E1000_RXD_STAT_EOP 0x02u

struct e1000_rx_desc {
    k_u64_t address;
    k_u16_t length;
    k_u16_t checksum;
    k_u8_t status;
    k_u8_t errors;
    k_u16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    k_u64_t address;
    k_u16_t length;
    k_u8_t cso;
    k_u8_t command;
    k_u8_t status;
    k_u8_t css;
    k_u16_t special;
} __attribute__((packed));

/* The rings: sixteen receive descriptors of one buffer each and eight
 * transmit ones. A frame of this stack is the largest ethernet frame, and the
 * buffers are 2048 bytes, so a descriptor is one frame and never a chain. */
#define E1000_RX_DESCRIPTORS 16u
#define E1000_TX_DESCRIPTORS 8u
#define E1000_BUFFER_SIZE 2048u

/* The window of BAR0 of the card: the size a PCI function of this family
 * asks for, which is also the size of the map. */
#define E1000_WINDOW_SIZE 0x20000u

/* The waits of this file are counted and not measured: the init of the driver
 * runs before the timer of the machine (src/port_x86_64.c), where the clock of
 * the port is still at zero, so a deadline of k_port_uptime_ms() would never
 * pass and a card that did not answer would hold the boot for ever. The budget
 * is far above what a card of QEMU or of a laptop needs for one register. */
#define E1000_WAIT_COUNT 100000u

/* The send path runs with the timer of the machine behind it, so the wait for
 * a descriptor the card gave back is a deadline of the clock. */
#define E1000_TX_TIMEOUT_MS 100u

static volatile k_u32_t *g_regs;
static k_u8_t g_mac[6];
static bool g_present;
static bool g_link;

static struct e1000_rx_desc *g_rx_ring;
static struct e1000_tx_desc *g_tx_ring;
static k_u8_t *g_rx_buffers;
static k_u8_t *g_tx_buffer;
static k_u16_t g_rx_next;
static k_u16_t g_tx_next;

static k_u64_t g_rx_frames;
static k_u64_t g_tx_frames;
static k_u64_t g_rx_dropped;

/* The operations the stack reads once the card is up (the bottom of this
 * file): the driver registers them from the init, which is above them. */
static const struct port_x86_64_nic_ops g_e1000_ops;

/* --- the register file ---------------------------------------------------- */

static k_u32_t e1000_read(k_u16_t offset)
{
    return g_regs[offset / 4u];
}

static void e1000_write(k_u16_t offset, k_u32_t value)
{
    g_regs[offset / 4u] = value;
}

/* --- the NVM -------------------------------------------------------------- */

/*
 * One word of the NVM. The card of this family keeps its MAC in the first
 * three words, and the register completes the read before the next
 * instruction on QEMU; the loop is for a card of a person, where the read is
 * a few microseconds of the flash behind the controller.
 */
static bool e1000_nvm_read(k_u8_t address, k_u16_t *value)
{
    k_u32_t count = E1000_WAIT_COUNT;

    e1000_write(E1000_EERD, ((k_u32_t)address << E1000_EERD_SHIFT) | E1000_EERD_START);

    for (;;) {
        k_u32_t answer = e1000_read(E1000_EERD);

        if ((answer & E1000_EERD_DONE) != 0u) {
            *value = (k_u16_t)(answer >> 16);
            return true;
        }

        if (count-- == 0u) {
            return false;
        }

        __asm__ volatile("pause");
    }
}

static bool e1000_read_mac(k_u8_t mac[6])
{
    k_u16_t word;
    k_size_t i;

    for (i = 0; i < 3u; i++) {
        if (!e1000_nvm_read((k_u8_t)i, &word)) {
            return false;
        }

        mac[i * 2u] = (k_u8_t)(word & 0xFFu);
        mac[i * 2u + 1u] = (k_u8_t)(word >> 8);
    }

    /* A MAC of all zeroes and one of all ones are what a card without an NVM
     * answers: neither names a station. */
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0u) {
        return false;
    }

    if ((mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) == 0xFFu) {
        return false;
    }

    return true;
}

/* --- the rings ------------------------------------------------------------ */

/* A block of the heap, aligned up to `alignment`: the descriptor rings are
 * read by the device and its addresses are the ones the heap gave, so the
 * alignment is a property of the format and not a preference. The block
 * itself is never freed: the rings live as long as the machine does. */
static void *e1000_alloc(k_size_t size, k_size_t alignment)
{
    k_u8_t *raw = (k_u8_t *)k_port_alloc(size + alignment, false);
    k_u64_t address;

    if (raw == NULL) {
        return NULL;
    }

    address = ((k_u64_t)(uintptr_t)raw + (k_u64_t)alignment - 1u) & ~((k_u64_t)alignment - 1u);

    memset((void *)(uintptr_t)address, 0, size);

    return (void *)(uintptr_t)address;
}

static bool e1000_rings_alloc(void)
{
    k_size_t i;

    g_rx_ring = (struct e1000_rx_desc *)e1000_alloc(
        (k_size_t)E1000_RX_DESCRIPTORS * sizeof(struct e1000_rx_desc), 128u);
    g_tx_ring = (struct e1000_tx_desc *)e1000_alloc(
        (k_size_t)E1000_TX_DESCRIPTORS * sizeof(struct e1000_tx_desc), 128u);
    g_rx_buffers = (k_u8_t *)e1000_alloc((k_size_t)E1000_RX_DESCRIPTORS * E1000_BUFFER_SIZE, 16u);
    g_tx_buffer = (k_u8_t *)e1000_alloc(E1000_BUFFER_SIZE, 16u);

    if (g_rx_ring == NULL || g_tx_ring == NULL || g_rx_buffers == NULL || g_tx_buffer == NULL) {
        kerror("e1000e0: no memory for the rings");
        return false;
    }

    for (i = 0; i < E1000_RX_DESCRIPTORS; i++) {
        g_rx_ring[i].address = port_x86_64_bus_phys(g_rx_buffers + i * (k_size_t)E1000_BUFFER_SIZE);
    }

    return true;
}

/*
 * Program the rings after the card has been reset. The receive ring is given
 * to the device whole - the tail is the last descriptor the driver owns, and
 * every buffer before it is the card's - while the transmit ring starts empty
 * at zero.
 */
static void e1000_rings_start(void)
{
    k_u64_t rx_ring = port_x86_64_bus_phys(g_rx_ring);
    k_u64_t tx_ring = port_x86_64_bus_phys(g_tx_ring);

    g_rx_next = 0u;
    g_tx_next = 0u;

    e1000_write(E1000_RDBAL, (k_u32_t)(rx_ring & 0xFFFFFFFFu));
    e1000_write(E1000_RDBAH, (k_u32_t)(rx_ring >> 32));
    e1000_write(E1000_RDLEN, E1000_RX_DESCRIPTORS * (k_u32_t)sizeof(struct e1000_rx_desc));
    e1000_write(E1000_RDH, 0u);
    e1000_write(E1000_RDT, E1000_RX_DESCRIPTORS - 1u);

    e1000_write(E1000_TDBAL, (k_u32_t)(tx_ring & 0xFFFFFFFFu));
    e1000_write(E1000_TDBAH, (k_u32_t)(tx_ring >> 32));
    e1000_write(E1000_TDLEN, E1000_TX_DESCRIPTORS * (k_u32_t)sizeof(struct e1000_tx_desc));
    e1000_write(E1000_TDH, 0u);
    e1000_write(E1000_TDT, 0u);
}

/* --- the init of the card ------------------------------------------------- */

static bool e1000_probe(struct port_x86_64_pci_device *card)
{
    k_size_t i;

    /*
     * The walk is by the identifiers and not by the class: the card of
     * virtio is a function of the class of the ethernet too, and a driver
     * that took it because the class matched would leave the machine with a
     * virtqueue it cannot read. A card of the family is a yes, and the walk of
     * the bus is cheap enough to ask for each of the ids.
     */
    for (i = 0; i < sizeof(g_e1000_ids) / sizeof(g_e1000_ids[0]); i++) {
        if (port_x86_64_pci_find(E1000_VENDOR_INTEL, g_e1000_ids[i], card)) {
            return true;
        }
    }

    return false;
}

static bool e1000_map(const struct port_x86_64_pci_device *card)
{
    k_u32_t bar = port_x86_64_pci_read32(card, PCI_BAR0);
    k_u64_t offset;
    k_u64_t window;

    if ((bar & PCI_BAR_IO) != 0u) {
        kerror("e1000e0: BAR0 is not a window of memory");
        return false;
    }

    /* The low four bits of the register are flags and the address is the
     * rest of it; the map is taken to whole 2 MiB pages, so the window is the
     * page the BAR starts in and the card is at its offset inside the page. */
    window = (k_u64_t)(bar & ~0x0Fu);
    offset = window & 0x1FFFFFu;

    if (port_x86_64_paging_map_mmio(PORT_X86_64_E1000_VIRT, window - offset,
                                    (k_size_t)(offset + E1000_WINDOW_SIZE)) == 0u) {
        kerror("e1000e0: the window of the card is not in the map of the port");
        return false;
    }

    g_regs = (volatile k_u32_t *)(uintptr_t)(PORT_X86_64_E1000_VIRT + offset);

    return true;
}

static bool e1000_reset(void)
{
    k_u32_t count = E1000_WAIT_COUNT;

    e1000_write(E1000_IMC, 0xFFFFFFFFu);

    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);

    while ((e1000_read(E1000_CTRL) & E1000_CTRL_RST) != 0u) {
        if (count-- == 0u) {
            kerror("e1000e0: the card did not come out of its reset");
            return false;
        }

        __asm__ volatile("pause");
    }

    /* The link is forced up and the receive and transmit halves stay off
     * until their rings are there. */
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU);
    e1000_write(E1000_RCTL, 0u);
    e1000_write(E1000_TCTL, 0u);

    return true;
}

static void e1000_filter_set(void)
{
    k_u32_t rah;

    e1000_write(E1000_RAL0, (k_u32_t)g_mac[0] | ((k_u32_t)g_mac[1] << 8) |
                                ((k_u32_t)g_mac[2] << 16) | ((k_u32_t)g_mac[3] << 24));
    rah = (k_u32_t)g_mac[4] | ((k_u32_t)g_mac[5] << 8) | E1000_RAH_AV;
    e1000_write(E1000_RAH0, rah);
}

static void e1000_start(void)
{
    /* A card that is up takes every frame of the segment, which is what a
     * machine of a boot wants; the filter above is what a card of a person
     * uses, and the promiscuous bits are the answer for the one whose NVM did
     * not name a MAC. */
    k_u32_t rctl = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC;

    if ((g_mac[0] & 1u) != 0u) {
        rctl |= E1000_RCTL_UPE | E1000_RCTL_MPE;
    }

    e1000_write(E1000_RCTL, rctl);
    e1000_write(E1000_TCTL,
                E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD | E1000_TCTL_RTLC);
    e1000_write(E1000_TIPG, E1000_TIPG_VALUE);

    /* The interrupt mask stays empty: the card is polled and a line of it
     * would wake a task that is not waiting for a frame. The status is read
     * once so that the bits of the reset are gone. */
    e1000_write(E1000_IMS, 0u);
    (void)e1000_read(E1000_ICR);
}

void port_x86_64_e1000e_init(void)
{
    struct port_x86_64_pci_device card;

    if (!e1000_probe(&card)) {
        return;
    }

    port_x86_64_pci_enable(&card, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    if (!e1000_map(&card)) {
        return;
    }

    if (!e1000_reset()) {
        return;
    }

    if (!e1000_read_mac(g_mac)) {
        kerror("e1000e0: the NVM of the card does not name a MAC");
        return;
    }

    if (!e1000_rings_alloc()) {
        return;
    }

    e1000_filter_set();
    e1000_rings_start();
    e1000_start();

    g_link = (e1000_read(E1000_STATUS) & E1000_STATUS_LU) != 0u;
    g_present = true;

    kinfo("e1000e0: %02x:%02x:%02x:%02x:%02x:%02x, %s", (unsigned)g_mac[0], (unsigned)g_mac[1],
          (unsigned)g_mac[2], (unsigned)g_mac[3], (unsigned)g_mac[4], (unsigned)g_mac[5],
          g_link ? "link up" : "no link");

    port_x86_64_nic_register(&g_e1000_ops);
}

const char *port_x86_64_e1000e_name(void)
{
    return g_present ? "e1000e0" : NULL;
}

/* --- the frames ----------------------------------------------------------- */

static bool e1000_nic_mac(k_u8_t mac[6])
{
    if (!g_present || mac == NULL) {
        return false;
    }

    memcpy(mac, g_mac, sizeof(g_mac));

    return true;
}

static bool e1000_nic_link(void)
{
    if (!g_present) {
        return false;
    }

    g_link = (e1000_read(E1000_STATUS) & E1000_STATUS_LU) != 0u;

    return g_link;
}

/*
 * One frame from the receive ring. The descriptor the driver owns is the one
 * at the tail - the card writes into the one after it and stops at the tail -
 * and the status word of the descriptor is the card's "this buffer is full".
 * The buffer goes back to the card in the same visit, so the next frame of
 * the segment has a place to land.
 */
static bool e1000_nic_receive(void *buf, k_size_t size, k_size_t *length)
{
    struct e1000_rx_desc *descriptor;
    k_u16_t frame_length;

    if (!g_present || buf == NULL || length == NULL) {
        return false;
    }

    descriptor = &g_rx_ring[g_rx_next];

    if ((descriptor->status & E1000_RXD_STAT_DD) == 0u) {
        return false;
    }

    frame_length = descriptor->length;

    if (frame_length > size || (descriptor->status & E1000_RXD_STAT_EOP) == 0u) {
        g_rx_dropped++;
    } else {
        memcpy(buf, g_rx_buffers + (k_size_t)g_rx_next * E1000_BUFFER_SIZE, frame_length);
        *length = frame_length;
        g_rx_frames++;
    }

    descriptor->status = 0u;
    descriptor->length = 0u;

    e1000_write(E1000_RDT, g_rx_next);

    g_rx_next = (k_u16_t)((g_rx_next + 1u) % E1000_RX_DESCRIPTORS);

    return frame_length <= size;
}

/*
 * Give one frame to the card. The transmit ring has one buffer: the frame is
 * copied into it, the descriptor is filled with the whole frame and the tail
 * moves past it, which is the doorbell of the card. The descriptor is the
 * driver's again when the card writes the done bit, and a card that does not
 * write it in the budget is a card that is gone - the frame is reported as
 * lost instead of holding a task for ever.
 */
static bool e1000_nic_send(const void *frame, k_size_t length)
{
    struct e1000_tx_desc *descriptor;
    k_u64_t deadline;

    if (!g_present || frame == NULL || length == 0u || length > E1000_BUFFER_SIZE) {
        return false;
    }

    descriptor = &g_tx_ring[g_tx_next];

    memcpy(g_tx_buffer, frame, length);

    descriptor->address = port_x86_64_bus_phys(g_tx_buffer);
    descriptor->length = (k_u16_t)length;
    descriptor->cso = 0u;
    descriptor->command = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    descriptor->status = 0u;
    descriptor->css = 0u;
    descriptor->special = 0u;

    e1000_write(E1000_TDT, (k_u32_t)((g_tx_next + 1u) % E1000_TX_DESCRIPTORS));

    deadline = k_port_uptime_ms() + E1000_TX_TIMEOUT_MS;

    while ((descriptor->status & E1000_TXD_STAT_DD) == 0u) {
        if (k_port_uptime_ms() > deadline) {
            return false;
        }

        __asm__ volatile("pause");
    }

    g_tx_next = (k_u16_t)((g_tx_next + 1u) % E1000_TX_DESCRIPTORS);
    g_tx_frames++;

    return true;
}

static k_u64_t e1000_nic_rx_frames(void)
{
    return g_rx_frames;
}

static k_u64_t e1000_nic_tx_frames(void)
{
    return g_tx_frames;
}

static k_u64_t e1000_nic_rx_dropped(void)
{
    return g_rx_dropped;
}

/* The seam the stack of src/net.c reads (src/netdev.c). The card registers as
 * soon as it is up; a machine with no card registers nothing. */
static const struct port_x86_64_nic_ops g_e1000_ops = {
    .name = "e1000e0",
    .mac = e1000_nic_mac,
    .link = e1000_nic_link,
    .receive = e1000_nic_receive,
    .send = e1000_nic_send,
    .rx_frames = e1000_nic_rx_frames,
    .tx_frames = e1000_nic_tx_frames,
    .rx_dropped = e1000_nic_rx_dropped,
};
