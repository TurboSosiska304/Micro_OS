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
 * The memory of the rings is the memory of the kernel: the port keeps its
 * kernel in the identity map of the first gigabyte (ADR 0032 §1), so the
 * address of a descriptor is the address the device wants and
 * port_x86_64_bus_phys() is the one place that says so. The window of the
 * registers is behind the BAR and sits above that map, so it is mapped into
 * the top 2 GiB of the kernel before the first read, as the SATA controller
 * and the NVMe disk are.
 *
 * The rings are arrays of the kernel and not blocks of the heap. The init of
 * this driver is among the last things the port does before the timer, and a
 * driver that asks the allocator for its rings is a driver that can wait on a
 * lock of a machine that has no task yet - one class of hanging that a boot
 * of a card does not have to carry at all.
 *
 * The waits of this file are deadlines of the time stamp counter, not counts
 * of `pause`. The init runs before `port_x86_64_interrupts_init()`, where
 * `k_port_uptime_ms()` is a zero that never moves, but the TSC was measured
 * against the PIT at the start of the boot and it moves from the first
 * instruction. Every step of the init has a hard budget (`E1000_WAIT_MS`), so
 * a card that does not answer is one line of the log, a `return`, and a
 * machine that goes on to its shell: a wire that does not answer is not a
 * boot that stops.
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

/* The control register: the software reset, the bit that forces the link up
 * instead of letting the card negotiate it, and the bit that makes the card
 * stop being the master of the bus before it is reset. The status register
 * says whether the master is still there. */
#define E1000_CTRL_SLU 0x00000040u
#define E1000_CTRL_GIO_MASTER_DISABLE 0x00000004u
#define E1000_CTRL_RST 0x04000000u

#define E1000_STATUS_LU 0x00000002u
#define E1000_STATUS_GIO_ENABLE 0x00000008u

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

/*
 * The budget of one step of the init, in real milliseconds. The waits of this
 * file are counted in nothing else: the TSC is the clock of the boot and a
 * step that spends the budget is a card that is not answering.
 */
#define E1000_WAIT_MS 200u

/* The master of the bus has to be quiet before the reset, and the card says
 * so in the status register within a few microseconds of a real card. */
#define E1000_MASTER_WAIT_MS 100u

/*
 * The fallback of a machine whose TSC was not calibrated (the PIT of a
 * firmware that owns it): a count of pauses, ~25 nanoseconds each, so this is
 * about eighty milliseconds of patience on a processor of any age. It only
 * has to end.
 */
#define E1000_PAUSE_BUDGET 4000000u

/* The send path runs with the timer of the machine behind it, so the wait for
 * a descriptor the card gave back is a deadline of the clock. */
#define E1000_TX_TIMEOUT_MS 100u

/*
 * The power management capability of the configuration space: the status word
 * says whether the function has a list of capabilities at all, the byte at
 * 0x34 is the first of them, and a power capability is identified by 0x01
 * with the power state in the lowest two bits of the control word four bytes
 * after its head (PCI PM 1.2 §3.2).
 */
#define PCI_STATUS_REG 0x06u
#define PCI_STATUS_CAP_LIST 0x0010u
#define PCI_CAP_POINTER 0x34u
#define PCI_CAP_ID_POWER 0x01u
#define PCI_PM_CSR_POWER_MASK 0x0003u
#define PCI_PM_CSR_D0 0x0000u
#define PCI_CAP_GUARD 48u /* the longest list a function may have, as a guard */

static volatile k_u32_t *g_regs;
static k_u8_t g_mac[6];
static bool g_mapped;
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

/* --- the clock of the waits ----------------------------------------------- */

/*
 * The clock of the init is the time stamp counter. The port measured it
 * against the PIT before it made its own page tables (src/port_x86_64.c,
 * "the calibration of the processor clock"), so it is the one clock that runs
 * in the window between the boot and the timer. A machine whose measurement
 * failed answers zero, and every wait of this file then falls back to a count
 * of pauses: slower than the time asked for, and finite.
 */
static k_u64_t e1000_tsc_span_ms(k_u32_t ms)
{
    const struct port_x86_64_info *info = port_x86_64_info();

    if (info == NULL || info->cpu_hz == 0u) {
        return 0u;
    }

    return (info->cpu_hz / 1000u) * (k_u64_t)ms;
}

/* The milliseconds between a reading of the TSC and now; zero on a machine
 * whose frequency is not known. */
static k_u32_t e1000_ms_since(k_u64_t start)
{
    const struct port_x86_64_info *info = port_x86_64_info();

    if (info == NULL || info->cpu_hz == 0u) {
        return 0u;
    }

    return (k_u32_t)(((port_rdtsc() - start) * 1000u) / info->cpu_hz);
}

/* Spend `ms` milliseconds on the TSC, or on the count of pauses of a machine
 * without one. A reset of a card of this family is milliseconds of the
 * silicon, and a driver that reads the register file while it is in the reset
 * is a driver that reads an answer the card is not giving yet. */
static void e1000_delay_ms(k_u32_t ms)
{
    k_u64_t span = e1000_tsc_span_ms(ms);
    k_u64_t start;
    k_u32_t pauses;

    if (span == 0u) {
        for (pauses = 0; pauses < ms * 40000u; pauses++) {
            __asm__ volatile("pause");
        }

        return;
    }

    start = port_rdtsc();

    while (port_rdtsc() - start < span) {
        __asm__ volatile("pause");
    }
}

/* --- the register file ---------------------------------------------------- */

/*
 * The registers, and the two guards of them. An offset outside the window of
 * the card is not a register of it, and a window that the table of the moment
 * does not hold is a page fault of the kernel and not a frame lost: a driver
 * of a card is not allowed to take the machine down because a mapping of the
 * boot is not in the space of a task.
 */
static k_u32_t e1000_read(k_u16_t offset)
{
    if (!g_mapped || (k_u32_t)offset + 4u > E1000_WINDOW_SIZE) {
        return 0u;
    }

    return g_regs[offset / 4u];
}

static void e1000_write(k_u16_t offset, k_u32_t value)
{
    if (!g_mapped || (k_u32_t)offset + 4u > E1000_WINDOW_SIZE) {
        return;
    }

    g_regs[offset / 4u] = value;
}

/* The window of the card in the space of the moment: the mapping is made once
 * at the boot into the top 2 GiB that every space of this port shares, and
 * this call says whether the space that runs now has it. */
static bool e1000_window_ready(void)
{
    if (!g_present || !g_mapped) {
        return false;
    }

    return port_x86_64_paging_kernel_present((k_u64_t)(uintptr_t)g_regs & ~(k_u64_t)0xFFFu);
}

/*
 * Wait until `mask` of the word at `offset` is `set` (or clear), or the budget
 * of the step runs out. The last word read is handed back either way, and
 * `spent_ms` is the time the wait took when the machine has a clock to say it
 * (zero on one whose TSC is unknown).
 */
static bool e1000_wait_bit(k_u16_t offset, k_u32_t mask, bool set, k_u32_t budget_ms,
                           k_u32_t *value, k_u32_t *spent_ms)
{
    k_u64_t span = e1000_tsc_span_ms(budget_ms);
    k_u64_t start = port_rdtsc();
    k_u32_t pauses = 0;

    for (;;) {
        k_u32_t answer = e1000_read(offset);

        if (((answer & mask) != 0u) == set) {
            if (value != NULL) {
                *value = answer;
            }

            if (spent_ms != NULL) {
                *spent_ms = e1000_ms_since(start);
            }

            return true;
        }

        if (span != 0u) {
            if (port_rdtsc() - start >= span) {
                break;
            }
        } else if (pauses++ >= budget_ms * (E1000_PAUSE_BUDGET / E1000_WAIT_MS)) {
            break;
        }

        __asm__ volatile("pause");
    }

    if (value != NULL) {
        *value = e1000_read(offset);
    }

    if (spent_ms != NULL) {
        *spent_ms = span != 0u ? e1000_ms_since(start) : budget_ms;
    }

    return false;
}

static bool e1000_wait_set(k_u16_t offset, k_u32_t mask, k_u32_t budget_ms, k_u32_t *value,
                           k_u32_t *spent_ms)
{
    return e1000_wait_bit(offset, mask, true, budget_ms, value, spent_ms);
}

static bool e1000_wait_clear(k_u16_t offset, k_u32_t mask, k_u32_t budget_ms, k_u32_t *value,
                             k_u32_t *spent_ms)
{
    return e1000_wait_bit(offset, mask, false, budget_ms, value, spent_ms);
}

/* --- the NVM -------------------------------------------------------------- */

/*
 * One word of the NVM. The card of this family keeps its MAC in the first
 * three words, and the register completes the read before the next
 * instruction on QEMU; the wait is for a card of a person, where the read is
 * a few microseconds of the flash behind the controller and the card of a
 * laptop with the management engine awake is slower still. The budget is
 * `E1000_WAIT_MS` and a card that spends it is a card that does not answer.
 */
static bool e1000_nvm_read(k_u8_t address, k_u16_t *value)
{
    k_u32_t answer = 0u;

    e1000_write(E1000_EERD, ((k_u32_t)address << E1000_EERD_SHIFT) | E1000_EERD_START);

    if (!e1000_wait_set(E1000_EERD, E1000_EERD_DONE, E1000_WAIT_MS, &answer, NULL)) {
        kerror("e1000e0: the NVM did not answer the word %u (EERD 0x%08x)", (unsigned)address,
               (unsigned)answer);
        return false;
    }

    *value = (k_u16_t)(answer >> 16);

    return true;
}

/* A MAC of all zeroes and one of all ones are what a card without an NVM
 * answers: neither names a station. */
static bool e1000_mac_valid(const k_u8_t mac[6])
{
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0u) {
        return false;
    }

    if ((mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) == 0xFFu) {
        return false;
    }

    return true;
}

static bool e1000_read_mac_nvm(k_u8_t mac[6])
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

    return e1000_mac_valid(mac);
}

/*
 * The second door to the station: the filter the reset loaded. The card
 * copies the address out of its NVM into RA0 itself, and a laptop whose
 * management engine holds the flash busy answers that register while it does
 * not answer EERD. The address bits are the ones the driver writes back into
 * the same register, so a station read here is a station the card knows.
 */
static bool e1000_read_mac_filter(k_u8_t mac[6])
{
    k_u32_t ral;
    k_u32_t rah;

    if (!g_mapped) {
        return false;
    }

    ral = e1000_read(E1000_RAL0);
    rah = e1000_read(E1000_RAH0);

    if ((rah & E1000_RAH_AV) == 0u) {
        return false;
    }

    mac[0] = (k_u8_t)(ral & 0xFFu);
    mac[1] = (k_u8_t)((ral >> 8) & 0xFFu);
    mac[2] = (k_u8_t)((ral >> 16) & 0xFFu);
    mac[3] = (k_u8_t)((ral >> 24) & 0xFFu);
    mac[4] = (k_u8_t)(rah & 0xFFu);
    mac[5] = (k_u8_t)((rah >> 8) & 0xFFu);

    return e1000_mac_valid(mac);
}

/* --- the rings ------------------------------------------------------------ */

/*
 * The memory of the rings and of the buffers, as arrays of the kernel. The
 * alignment is a property of the format of the descriptor - the card reads a
 * ring of 128-byte lines - and a static array is aligned by the compiler
 * without an allocator, without a lock and without a failure path: the bytes
 * are there before the init runs, and the address the card is given is the
 * identity map of them (ADR 0032 §1).
 */
#define E1000_RING_ALIGNMENT 128u

static struct e1000_rx_desc g_rx_descriptors[E1000_RX_DESCRIPTORS]
    __attribute__((aligned(E1000_RING_ALIGNMENT)));
static struct e1000_tx_desc g_tx_descriptors[E1000_TX_DESCRIPTORS]
    __attribute__((aligned(E1000_RING_ALIGNMENT)));
static k_u8_t g_rx_buffer_memory[E1000_RX_DESCRIPTORS * E1000_BUFFER_SIZE]
    __attribute__((aligned(16)));
static k_u8_t g_tx_buffer_memory[E1000_BUFFER_SIZE] __attribute__((aligned(16)));

static bool e1000_rings_alloc(void)
{
    k_size_t i;

    g_rx_ring = g_rx_descriptors;
    g_tx_ring = g_tx_descriptors;
    g_rx_buffers = g_rx_buffer_memory;
    g_tx_buffer = g_tx_buffer_memory;

    memset(g_rx_ring, 0, sizeof(g_rx_descriptors));
    memset(g_tx_ring, 0, sizeof(g_tx_descriptors));
    memset(g_rx_buffers, 0, sizeof(g_rx_buffer_memory));
    memset(g_tx_buffer, 0, sizeof(g_tx_buffer_memory));

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

/* --- the power state of the card ------------------------------------------ */

/*
 * A function of the bus that the firmware left in D3hot does not decode its
 * window of memory: a read of a register of it answers all ones (or hangs the
 * root port of a machine whose link is off), and the reset of a card that is
 * asleep wakes nothing. A driver of a laptop therefore puts the card back into
 * D0 through the power management capability before it touches a register,
 * and every machine of QEMU answers the same walk: the list is absent on a
 * function without the capability, and the card is D0 already.
 */
static void e1000_power_d0(const struct port_x86_64_pci_device *card)
{
    k_u16_t status = port_x86_64_pci_read16(card, PCI_STATUS_REG);
    k_u8_t pointer;
    k_u8_t guard;

    if ((status & PCI_STATUS_CAP_LIST) == 0u) {
        kinfo("e1000e: the card has no list of capabilities, the power state is the firmware's");
        return;
    }

    pointer = (k_u8_t)(port_x86_64_pci_read32(card, PCI_CAP_POINTER) & 0xFCu);

    for (guard = 0; guard < PCI_CAP_GUARD && pointer >= 0x40u; guard++) {
        k_u32_t head = port_x86_64_pci_read32(card, pointer);

        if ((k_u8_t)(head & 0xFFu) == PCI_CAP_ID_POWER) {
            k_u8_t csr_offset = (k_u8_t)(pointer + 4u);
            k_u16_t csr = port_x86_64_pci_read16(card, csr_offset);
            k_u16_t state = (k_u16_t)(csr & PCI_PM_CSR_POWER_MASK);

            if (state != PCI_PM_CSR_D0) {
                /* A card of a laptop that the firmware left asleep does not
                 * decode its window of memory, and its reset wakes nothing.
                 * The write of a state leaves the other bits of the word
                 * alone; a device that was in D3hot wants ten milliseconds of
                 * the silicon before its window answers again (PCI PM 1.2
                 * §5.6.1). */
                kinfo("e1000e: the firmware left the card in D%u (PMCSR 0x%04x)", (unsigned)state,
                      (unsigned)csr);

                port_x86_64_pci_write16(card, csr_offset, (k_u16_t)(csr & ~PCI_PM_CSR_POWER_MASK));
                e1000_delay_ms(10);
                csr = port_x86_64_pci_read16(card, csr_offset);
            }

            kinfo("e1000e: the power state of the card is D%u (PMCSR 0x%04x)",
                  (unsigned)(csr & PCI_PM_CSR_POWER_MASK), (unsigned)csr);
            return;
        }

        pointer = (k_u8_t)((head >> 8) & 0xFCu);
    }

    kinfo("e1000e: the list of capabilities names no power capability");
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
    k_u32_t high = 0u;
    k_u64_t offset;
    k_u64_t window;

    if ((bar & PCI_BAR_IO) != 0u) {
        kerror("e1000e: BAR0 of the card is not a window of memory");
        return false;
    }

    /*
     * A 64-bit window keeps its upper half in the register after the first
     * one (bits 2:1 of the low half are 10b), and a laptop of a firmware that
     * put the card above 4 GiB has its address there. A driver that reads
     * only the low half maps a page of something else, and the first register
     * it reads is the page fault the report of the laptop showed.
     */
    if (((bar >> 1) & 3u) == 2u) {
        high = port_x86_64_pci_read32(card, PCI_BAR1);
    }

    window = ((k_u64_t)high << 32) | (k_u64_t)(bar & ~0x0Fu);

    kinfo("e1000e: BAR0 is 0x%x:%08x, %s", (unsigned)high, (unsigned)(bar & ~0x0Fu),
          high != 0u || ((bar >> 1) & 3u) == 2u ? "a 64-bit window" : "a 32-bit window");

    if (window == 0u || (k_u32_t)(bar & ~0x0Fu) == 0xFFFFFFF0u) {
        kerror("e1000e: the card did not name a window of memory");
        return false;
    }

    /* The low four bits of the register are flags and the address is the
     * rest of it; the map is taken to whole 2 MiB pages, so the window is the
     * page the BAR starts in and the card is at its offset inside the page. */
    offset = window & 0x1FFFFFu;

    if (port_x86_64_paging_map_mmio(PORT_X86_64_E1000_VIRT, window - offset,
                                    (k_size_t)(offset + E1000_WINDOW_SIZE)) == 0u) {
        kerror("e1000e: the window of BAR0 at 0x%x:%08x is not in the map of the port",
               (unsigned)(window >> 32), (unsigned)(window & 0xFFFFFFFFu));
        return false;
    }

    g_regs = (volatile k_u32_t *)(uintptr_t)(PORT_X86_64_E1000_VIRT + offset);
    g_mapped = true;

    /* A window that answers all ones is a window of nothing: the card did not
     * decode the address, and a driver that programs rings through it would
     * wait for a device that is not there. */
    if (e1000_read(E1000_STATUS) == 0xFFFFFFFFu && e1000_read(E1000_CTRL) == 0xFFFFFFFFu) {
        kerror("e1000e: the window of BAR0 answers all ones, the card is not there");
        g_mapped = false;
        return false;
    }

    kinfo("e1000e: the window of BAR0 is %u KiB, mapped", (unsigned)(E1000_WINDOW_SIZE / 1024u));

    return true;
}

/*
 * The card stops being the master of the bus before it is reset, and that is
 * not a courtesy. The datasheet of this family - the 82574 of QEMU and every
 * ICH and PCH part after it - is explicit about the price of skipping the
 * step: "Prevent the PCI-E bus from sticking if there is no TLP connection on
 * the last TLP read/write transaction when MAC is reset". A card that the
 * firmware of a laptop left receiving frames is a card with a transaction in
 * flight, and a reset in the middle of it is a bus of the root port that
 * never completes another cycle: the next read of the register file never
 * returns, and the machine stays on with the last line of the log and no
 * panic. That is the shape of the report from the laptop, and this is the
 * step its boot was missing.
 *
 * The master bit of the command register goes first (the function stops
 * beginning transactions of its own), then the GIO master of the card, and
 * the status register says when the last one is gone. The wait is bounded
 * like every other wait of this file, and a card that keeps the bit up is a
 * line of the log and a reset that is attempted anyway: a firmware that does
 * not implement the bit leaves the machine where an older driver left it.
 */
static void e1000_master_off(const struct port_x86_64_pci_device *card)
{
    k_u32_t status = 0u;
    k_u32_t spent = 0u;

    port_x86_64_pci_write16(
        card, PCI_COMMAND,
        (k_u16_t)(port_x86_64_pci_read16(card, PCI_COMMAND) & ~PCI_COMMAND_BUS_MASTER));

    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_GIO_MASTER_DISABLE);

    if (e1000_wait_clear(E1000_STATUS, E1000_STATUS_GIO_ENABLE, E1000_MASTER_WAIT_MS, &status,
                         &spent)) {
        kinfo("e1000e: the master of the bus is off in %u ms (STATUS 0x%08x)", (unsigned)spent,
              (unsigned)status);
    } else {
        kwarn("e1000e: the master of the bus did not go off in %u ms (STATUS 0x%08x), reset anyway",
              (unsigned)E1000_MASTER_WAIT_MS, (unsigned)status);
    }
}

static bool e1000_reset(k_u32_t *spent_ms)
{
    k_u32_t ctrl = 0u;

    e1000_write(E1000_IMC, 0xFFFFFFFFu);

    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);

    /*
     * The reset of a card of this family is milliseconds of the silicon, and
     * a read of the register file while it runs is a read of a device that is
     * not answering yet: the driver of Linux sleeps here for the same reason.
     * The delay is a real time and not a count of pauses, because a count of
     * pauses is a different length on every processor and on a card that is
     * behind the management engine it is far too short.
     */
    e1000_delay_ms(10);

    if (!e1000_wait_clear(E1000_CTRL, E1000_CTRL_RST, E1000_WAIT_MS, &ctrl, spent_ms)) {
        kerror("e1000e0: the card did not come out of its reset in %u ms (CTRL 0x%08x)",
               (unsigned)E1000_WAIT_MS, (unsigned)ctrl);
        return false;
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

/*
 * The init of the card, one step and one line of the log at a time. Every
 * line names the step that the line before it left behind, so a boot that
 * stops on this card says on its screen which register it was asking, and a
 * step that runs out of its budget returns: the wire is unavailable and the
 * machine goes on to its shell.
 */
void port_x86_64_e1000e_init(void)
{
    struct port_x86_64_pci_device card;
    k_u32_t spent = 0u;
    k_u32_t ctrl = 0u;
    k_u32_t status = 0u;

    if (!e1000_probe(&card)) {
        kinfo("e1000e: no card of the family on the PCI bus");
        return;
    }

    kinfo("e1000e: the card is at %02x:%02x.%x", (unsigned)card.bus, (unsigned)card.slot,
          (unsigned)card.function);

    e1000_power_d0(&card);

    port_x86_64_pci_enable(&card, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    kinfo("e1000e: the command register is 0x%04x",
          (unsigned)port_x86_64_pci_read16(&card, PCI_COMMAND));

    if (!e1000_map(&card)) {
        return;
    }

    status = e1000_read(E1000_STATUS);
    ctrl = e1000_read(E1000_CTRL);

    kinfo("e1000e: the card before the reset: STATUS 0x%08x, CTRL 0x%08x", (unsigned)status,
          (unsigned)ctrl);

    e1000_master_off(&card);

    if (!e1000_reset(&spent)) {
        return;
    }

    /* The reset of the MAC leaves the command register alone, and the master
     * of the bus is the one the rings of this driver are given back. */
    port_x86_64_pci_enable(&card, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    kinfo("e1000e: reset ok in %u ms, CTRL 0x%08x", (unsigned)spent,
          (unsigned)e1000_read(E1000_CTRL));

    if (!e1000_read_mac_nvm(g_mac)) {
        /* The NVM did not name the station. The filter the reset loaded is
         * the card's own copy of it, and a card that answers there is a card
         * of a wire; a card that answers nowhere is a line of the log and a
         * machine that goes on without it. */
        if (!e1000_read_mac_filter(g_mac)) {
            kerror("e1000e0: the NVM and the filter of the card name no MAC");
            return;
        }

        kwarn("e1000e0: the NVM of the card did not answer, the MAC is the one of its filter");
    }

    kinfo("e1000e: mac ok, %02x:%02x:%02x:%02x:%02x:%02x", (unsigned)g_mac[0], (unsigned)g_mac[1],
          (unsigned)g_mac[2], (unsigned)g_mac[3], (unsigned)g_mac[4], (unsigned)g_mac[5]);

    if (!e1000_rings_alloc()) {
        return;
    }

    kinfo("e1000e: rings ok, the receive ring at 0x%08x (phys 0x%08x), %u descriptors",
          (unsigned)(uintptr_t)g_rx_ring, (unsigned)port_x86_64_bus_phys(g_rx_ring),
          (unsigned)E1000_RX_DESCRIPTORS);

    e1000_filter_set();
    e1000_rings_start();
    e1000_start();

    g_link = (e1000_read(E1000_STATUS) & E1000_STATUS_LU) != 0u;
    g_present = true;

    kinfo("e1000e: start ok, RCTL 0x%08x, TCTL 0x%08x, STATUS 0x%08x",
          (unsigned)e1000_read(E1000_RCTL), (unsigned)e1000_read(E1000_TCTL),
          (unsigned)e1000_read(E1000_STATUS));

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
    if (!e1000_window_ready() || mac == NULL) {
        return false;
    }

    memcpy(mac, g_mac, sizeof(g_mac));

    return true;
}

static bool e1000_nic_link(void)
{
    if (!e1000_window_ready()) {
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

    if (!e1000_window_ready() || buf == NULL || length == NULL) {
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

    if (!e1000_window_ready() || frame == NULL || length == 0u || length > E1000_BUFFER_SIZE) {
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
