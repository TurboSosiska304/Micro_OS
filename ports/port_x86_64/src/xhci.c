/*
 * Micro OS — the host controller of the USB of the x86-64 port (ADR 0040).
 *
 * The machine of a laptop has no PS/2 port its firmware will share: the
 * keyboard is a device of the USB, and the only way to it is the host
 * controller of the PCI bus (xHCI, class 0x0C / 0x03, interface 0x30). The
 * controller is a ring machine: the driver writes a command into a ring of
 * TRBs (transfer request blocks) and rings the doorbell of the controller,
 * the controller writes an event into the event ring and raises an
 * interrupt, and the answer of a command is an event that names the TRB that
 * asked it. Nothing of that is a bus of the PC: the controller reads and
 * writes the memory of the machine itself, so every address of a ring or of
 * a buffer is a physical address (ADR 0032), and the rings here live in the
 * image of the kernel, where port_x86_64_bus_phys() tells the two halves
 * apart.
 *
 * The three layers of the USB are three files in the specification and one
 * driver here: this file is the host controller (the slots, the rings, the
 * commands and the ports), the enumeration of one device at a time, and the
 * class of the keyboard of the boot protocol of HID, which is the device a
 * laptop of this release needs. A stick (mass storage) and a dongle (CDC)
 * are the second half of the line and are not here; a device of another
 * class is enumerated to the point of its configuration descriptor, is
 * found not to be a keyboard, and gives its slot back.
 *
 * Nothing here is a fact of the PS/2: the reports of the keyboard are eight
 * bytes of usages of the HID page, the portable decoder of the kernel
 * (kernel/input_hid.c) turns them into the keys and the characters of the
 * machine, and the rings of this file are shaped like the ones of src/ps2.c
 * so that the console and /dev/input0 read one kind of event whichever
 * keyboard a person has under the hands. The mouse of the boot protocol is
 * the second user of that decoder family: its report is three or four bytes
 * of differences and buttons, and the events it makes are the pointer events
 * of the PS/2 mouse.
 *
 * A device is not always in a port of the controller: the laptop of these
 * years puts its keyboard and its stick behind the hub of a dock. The hub is
 * the class 09, its ports are powered and polled, and a device behind it is
 * addressed with the route string of the slot context. The ports of the
 * controller and of the hubs are scanned after the boot, so a device that
 * arrives later is enumerated and one that leaves is taken away.
 *
 * The controller owns a line: the message of MSI or MSI-X of the PCI
 * function, or its legacy pin on a machine that kept the 8259. The handler
 * acknowledges the interrupter and wakes the console; the event ring itself
 * is read by the pump in the task that asked, because a second reader of the
 * ring in the handler would take a completion from under a command that is
 * waiting for it.
 */
#include "port_x86_64_xhci.h"

#include <string.h>

#include "kernel/console.h"
#include "kernel/input.h"
#include "kernel/input_hid.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/port/lock.h"
#include "kernel/port/mem.h"
#include "kernel/port/time.h"
#include "kernel/port_task.h"
#include "port_x86_64.h"
#include "port_x86_64_apic.h"
#include "port_x86_64_console.h"
#include "port_x86_64_input.h"
#include "port_x86_64_pci.h"

/* The stub of the line of the controller in the table of interrupts.S. */
extern void isr_irq_xhci(void);

/* --- the registers of the controller -------------------------------------- */

/* The capability registers, from the base of the BAR. */
#define XHCI_CAP_CAPLENGTH 0x00u
#define XHCI_CAP_HCIVERSION 0x02u
#define XHCI_CAP_HCSPARAMS1 0x04u
#define XHCI_CAP_HCSPARAMS2 0x08u
#define XHCI_CAP_DBOFF 0x14u
#define XHCI_CAP_RTSOFF 0x18u

/* The operational registers, from the base plus CAPLENGTH. */
#define XHCI_OP_USBCMD 0x00u
#define XHCI_OP_USBSTS 0x04u
#define XHCI_OP_CRCR 0x18u
#define XHCI_OP_DCBAAP 0x30u
#define XHCI_OP_CONFIG 0x38u
#define XHCI_OP_PORTS 0x400u /* PORTSC of the port 1, sixteen bytes a port */

/* The runtime registers, from the base plus RTSOFF: the interrupter 0 is the
 * first and starts at 0x20, and the four registers of it are these. */
#define XHCI_RT_INTR 0x20u
#define XHCI_RT_IMAN (XHCI_RT_INTR + 0x00u)
#define XHCI_RT_ERSTSZ (XHCI_RT_INTR + 0x08u)
#define XHCI_RT_ERSTBA (XHCI_RT_INTR + 0x10u)
#define XHCI_RT_ERDP (XHCI_RT_INTR + 0x18u)

#define XHCI_CMD_RUN 0x00000001u
#define XHCI_CMD_RESET 0x00000002u
#define XHCI_CMD_INTE 0x00000004u

#define XHCI_STS_HALTED 0x00000001u
#define XHCI_STS_ERROR 0x00000004u
#define XHCI_STS_EVENT 0x00000008u
#define XHCI_STS_CNR 0x00000800u

/* The interrupter: the pending bit of the interrupt and its enable, both of
 * the management register. The pending bit is written with a one to clear it,
 * as the end of an interrupt does. */
#define XHCI_IMAN_IP 0x00000001u
#define XHCI_IMAN_IE 0x00000002u
#define XHCI_ERDP_EHB 0x00000008u

/* The vector of the line of the controller: it sits above the window of the
 * 8259 and of the local APIC timer (0x20..0x2F, 0x50..0x51) and below the
 * door of the programs (0x80). */
#define XHCI_VECTOR 0x60u

/* The interrupt of the PCI function: the capability list of the
 * configuration space, the identifier of a message-signalled interrupt and
 * the address of the local APIC in the physical map of the machine. */
#define PCI_CAP_PTR 0x34u
#define PCI_STATUS 0x06u
#define PCI_STATUS_CAP 0x0010u
#define PCI_CAP_ID_MSI 0x05u
#define PCI_CAP_ID_MSIX 0x11u
#define PCI_MSI_CTRL 0x02u
#define PCI_MSI_ADDR 0x04u
#define PCI_MSI_ADDR_HIGH 0x08u
#define PCI_MSI_DATA 0x0Cu
#define PCI_MSI_64BIT 0x0080u
#define PCI_MSI_ENABLE 0x0001u
#define PCI_MSIX_TABLE 0x04u
#define PCI_MSIX_ENABLE 0x8000u
#define PCI_MSIX_ENTRY 16u
#define PCI_MSI_CAP_MAX 48u
#define PCI_APIC_ADDRESS 0xFEE00000u

/* One port: the bits of PORTSC this driver reads and writes. */
#define XHCI_PORT_CCS 0x00000001u
#define XHCI_PORT_PED 0x00000002u
#define XHCI_PORT_PR 0x00000010u
#define XHCI_PORT_PLS_MASK 0x000001E0u
#define XHCI_PORT_PP 0x00000200u
#define XHCI_PORT_SPEED_SHIFT 10u
#define XHCI_PORT_SPEED_MASK 0x00003C00u
#define XHCI_PORT_CHANGE 0x00FE0000u /* CSC..CEC, all write-one-to-clear */
#define XHCI_PORT_PRC 0x00200000u
#define XHCI_PORT_CSC 0x00020000u

/* The speeds of a port, as PORTSC names them. */
#define XHCI_SPEED_FULL 1u
#define XHCI_SPEED_LOW 2u
#define XHCI_SPEED_HIGH 3u
#define XHCI_SPEED_SUPER 4u

/* The link states of PORTSC.PLS (bits 5..8). A USB3 port is enabled when it
 * is in U0; a port in Polling (7) has not finished its reset, and a port in
 * Inactive (6) has no partner. */
#define XHCI_PLS_U0 0u
#define XHCI_PLS_POLLING 7u
#define XHCI_PLS_INACTIVE 6u

/*
 * The waits of a port reset. The budget of the reset itself is the one of the
 * specification (500 ms, and a device behind a bad cable may use all of it),
 * the enable after PRC is the moment of the device, and the power of the port
 * is a rail that comes up in twenty milliseconds (§4.19.1).
 */
#define XHCI_PORT_POWER_MS 20u
#define XHCI_RESET_TIMEOUT_US 500000u
#define XHCI_RESET_ENABLE_US 100000u
#define XHCI_RESET_SETTLE_MS 10u
#define XHCI_RESET_ATTEMPTS 5u

/* --- the TRBs -------------------------------------------------------------- */

/* One transfer request block: sixteen bytes, a parameter, a status and a
 * control word. The type of it is the six bits of the control at 10. */
struct xhci_trb {
    k_u64_t param;
    k_u32_t status;
    k_u32_t control;
};

#define XHCI_TRB_TYPE(type) ((k_u32_t)(type) << 10)
#define XHCI_TRB_GET_TYPE(control) (((control) >> 10) & 0x3Fu)
#define XHCI_TRB_CYCLE 0x00000001u
#define XHCI_TRB_TC 0x00000002u /* a link that ends the ring */
#define XHCI_TRB_CHAIN 0x00000010u
#define XHCI_TRB_IOC 0x00000020u
#define XHCI_TRB_IDT 0x00000040u
#define XHCI_TRB_DIR_IN 0x00010000u
#define XHCI_TRB_SLOT(control) (((control) >> 24) & 0xFFu)
#define XHCI_TRB_ENDPOINT(control) (((control) >> 16) & 0x1Fu)
#define XHCI_TRB_CODE(status) (((status) >> 24) & 0xFFu)
#define XHCI_TRB_LENGTH(status) ((status) & 0x1FFFFu)

enum xhci_trb_type {
    XHCI_TRB_NORMAL = 1,
    XHCI_TRB_SETUP = 2,
    XHCI_TRB_DATA = 3,
    XHCI_TRB_STATUS = 4,
    XHCI_TRB_LINK = 6,
    XHCI_TRB_ENABLE_SLOT = 9,
    XHCI_TRB_ADDRESS_DEVICE = 11,
    XHCI_TRB_CONFIGURE_ENDPOINT = 12,
    XHCI_TRB_EVALUATE_CONTEXT = 13,
    XHCI_TRB_DISABLE_SLOT = 10,
    XHCI_TRB_TRANSFER = 32,
    XHCI_TRB_COMMAND_COMPLETION = 33,
    XHCI_TRB_PORT_STATUS = 34,
};

/* The codes of a completion this driver calls a success: a transfer that
 * moved every byte it asked for, and one that moved fewer (a short packet,
 * which is how a device says its answer is shorter than the room). */
#define XHCI_CODE_SUCCESS 1u
#define XHCI_CODE_SHORT 13u
#define XHCI_CODE_STALL 6u

/* The types of an endpoint of a device context. */
#define XHCI_EP_TYPE_CONTROL 4u
#define XHCI_EP_TYPE_INTERRUPT_IN 7u

/* --- the memory the controller reads -------------------------------------- */

#define XHCI_CMD_TRBS 16u
#define XHCI_EVENT_TRBS 64u
#define XHCI_EP_TRBS 8u
#define XHCI_DEVICES 8u
#define XHCI_HUBS 4u
#define XHCI_KBD_BUFFERS (XHCI_EP_TRBS - 1u)

/* The command ring and the event ring: sixteen bytes a TRB, aligned to the
 * sixty-four bytes the controller asks of a ring. */
static struct xhci_trb g_cmd_ring[XHCI_CMD_TRBS] __attribute__((aligned(64)));
static struct xhci_trb g_event_ring[XHCI_EVENT_TRBS] __attribute__((aligned(64)));

/* The table of the event ring segments: one segment, a physical address and
 * a size in TRBs. */
static k_u64_t g_erst[2] __attribute__((aligned(64)));

/* The array of the device contexts: the slot 0 is the array of the scratchpad
 * buffers of the controller. */
static k_u64_t g_dcbaa[65] __attribute__((aligned(64)));

/*
 * A slot of the driver: the number the controller gave it and the state of
 * the ring of its endpoint 0. The controller numbers its slots from one, and
 * the pool below is what the driver has contexts for; the slot of a device is
 * not its place in the pool, because a hub arrives before the devices behind
 * it and a device that leaves gives its number back.
 */
struct xhci_slot_state {
    bool used;
    k_u8_t slot;
    k_u32_t ep0_index;
    k_u32_t ep0_cycle;
};

static struct xhci_slot_state g_slot_pool[XHCI_DEVICES];

/* The context of a device, 2 KiB, and its ring of the endpoint 0. */
static k_u8_t g_device_ctx[XHCI_DEVICES][2048] __attribute__((aligned(64)));
static struct xhci_trb g_ep0_ring[XHCI_DEVICES][XHCI_EP_TRBS] __attribute__((aligned(64)));

/* The input context of a command: 32 bytes of the control context and 32
 * bytes of a context for the slot and each of the 31 endpoints. */
static k_u8_t g_input[1056] __attribute__((aligned(64)));

/* The buffer of a descriptor during the enumeration, and the report of the
 * keyboard: eight bytes, of which the controller writes as many as the
 * device sends. */
static k_u8_t g_desc[512] __attribute__((aligned(64)));
static k_u8_t g_kbd_report[XHCI_KBD_BUFFERS][K_INPUT_HID_REPORT] __attribute__((aligned(64)));
static struct xhci_trb g_kbd_ring[XHCI_EP_TRBS] __attribute__((aligned(64)));

/* The report of the mouse: three bytes of the boot protocol and the fourth
 * of the wheel, and its own ring of seven places. */
static k_u8_t g_mouse_report[XHCI_KBD_BUFFERS][K_INPUT_HID_MOUSE_REPORT]
    __attribute__((aligned(64)));
static struct xhci_trb g_mouse_ring[XHCI_EP_TRBS] __attribute__((aligned(64)));

/* The descriptor of a hub: the longest form of it, twelve bytes for a hub of
 * the SuperSpeed and nine of the other one with the variable tail. */
static k_u8_t g_hub_desc[16] __attribute__((aligned(64)));

/* The scratchpad buffers of a controller that asks for them: the array of
 * their addresses, which the controller reads, and the pages themselves,
 * which this driver allocates on the heap and keeps for the life of the
 * machine. */
/*
 * The scratchpad buffers a controller asks for (HCSPARAMS2, up to 1023 of
 * them). A controller of QEMU asks for none, and the driver of 1.7 was
 * written against it and kept eight: the controller of a laptop asks for 65
 * and the driver refused to start, with the sentence in the log and no USB at
 * all - no keyboard, no mouse, no stick. The memory is a page a buffer, which
 * a machine of 13 MB of heap does not notice.
 */
#define XHCI_SCRATCH_MAX 256u
static k_u64_t g_scratch_array[XHCI_SCRATCH_MAX] __attribute__((aligned(64)));
static void *g_scratch_raw[XHCI_SCRATCH_MAX];

/* --- the state of the driver ---------------------------------------------- */

static volatile k_u8_t *g_cap; /* the registers of the capability */
static volatile k_u8_t *g_op;  /* the operational registers */
static volatile k_u8_t *g_rt;  /* the runtime registers */
static volatile k_u8_t *g_db;  /* the doorbells */
static k_u32_t g_max_slots;
static k_u32_t g_max_ports;
static k_u32_t g_port_base;

static bool g_running; /* the rings are there and the controller runs */
static k_u32_t g_cmd_index;
static k_u32_t g_cmd_cycle;
static k_u32_t g_evt_index;
static k_u32_t g_evt_cycle;

/* The device of the USB is one at a time in a command: the enumeration and
 * the scan of the ports take this lock, and so does a transfer of a class,
 * because the command ring and the event ring are the state of the machine
 * and two writers of it are a lost completion. The boot takes it before the
 * scheduler exists, when nobody contends. */
static k_mutex_t g_xhci_lock = K_MUTEX_INITIALIZER;

/* Where a device hangs: its root port, its route string (the port numbers of
 * the hubs above it, four bits a tier), how many hubs that is, and the slot
 * of the hub it is attached to (0 for the root). */
struct xhci_where {
    k_u8_t root_port;
    k_u8_t depth;
    k_u8_t parent_slot;
    k_u32_t route;
};

/* The keyboard of the machine: the slot and the endpoint of the reports, and
 * the producer of the ring of them. */
static struct {
    bool present;
    k_u8_t slot;
    k_u8_t dci;
    k_u8_t interface;
    k_u16_t max_packet;
    k_u32_t index;
    k_u32_t cycle;
    struct xhci_where where;
} g_keyboard;

static struct k_input_hid_state g_hid;

/* The mouse of the machine: the same shape, with a report of three bytes (the
 * fourth of the wheel is read when the device sends it) and a decoder of the
 * boot protocol of a mouse. */
static struct {
    bool present;
    k_u8_t slot;
    k_u8_t dci;
    k_u8_t interface;
    k_u16_t max_packet;
    k_u32_t index;
    k_u32_t cycle;
    struct xhci_where where;
} g_mouse;

static struct k_input_hid_mouse_state g_mouse_hid;

/* A hub of the machine: its slot, where it hangs, the ports it has and
 * whether it speaks the SuperSpeed protocol (whose port status has another
 * layout). The ports are polled; the interrupt endpoint of the hub is not
 * configured, because the ports of a dock change at the pace of a hand. */
struct xhci_hub {
    bool used;
    k_u8_t slot;
    k_u8_t root_port;
    k_u32_t route;
    k_u8_t depth;
    k_u8_t parent_slot;
    k_u8_t ports;
    bool superspeed;
    k_u32_t scanned_ms;
};

static struct xhci_hub g_hubs[XHCI_HUBS];

/* The scan of the ports: the root ports of the controller at every pump (a
 * register read), and the ports below a hub four times a second (a control
 * transfer each, which is the price of a hub whose interrupt endpoint this
 * driver does not read). */
#define XHCI_HUB_SCAN_MS 250u
static k_u32_t g_next_hub_scan_ms;

/* The controller controls the power of its ports (HCCPARAMS1.PPC). A
 * controller without it powers every port itself, and PP reads zero for
 * ever: writing it or waiting for it on such a machine is a wait for a bit
 * that no register of it holds. */
static bool g_ppc;

/* The line of the controller: true when the function gave this driver an
 * interrupt, and the vector it arrived on. */
static bool g_irq;
static k_u32_t g_irq_vector;
static bool g_irq_first_seen;
static k_u32_t g_irq_count;

/* The rings of the characters and the events, shaped as the ones of the
 * PS/2: a task reads them and the console is told that a key is there. */
#define XHCI_INPUT_RING 64u

static volatile struct input_event g_in_events[XHCI_INPUT_RING];
static volatile k_u32_t g_in_event_head;
static volatile k_u32_t g_in_event_tail;
static volatile k_u8_t g_in_chars[XHCI_INPUT_RING];
static volatile k_u32_t g_in_char_head;
static volatile k_u32_t g_in_char_tail;
static bool g_capture;

/* --- the registers, one access a line -------------------------------------- */

static k_u32_t op_read(k_u32_t offset)
{
    return *(volatile k_u32_t *)(g_op + offset);
}

static void op_write(k_u32_t offset, k_u32_t value)
{
    *(volatile k_u32_t *)(g_op + offset) = value;
}

static void op_write64(k_u32_t offset, k_u64_t value)
{
    op_write(offset, (k_u32_t)value);
    op_write(offset + 4u, (k_u32_t)(value >> 32));
}

static void rt_write(k_u32_t offset, k_u32_t value)
{
    *(volatile k_u32_t *)(g_rt + offset) = value;
}

static void rt_write64(k_u32_t offset, k_u64_t value)
{
    rt_write(offset, (k_u32_t)value);
    rt_write(offset + 4u, (k_u32_t)(value >> 32));
}

/* --- the pool of the slots -------------------------------------------------- */

static struct xhci_slot_state *slot_state_of(k_u32_t slot)
{
    k_u32_t index;

    for (index = 0; index < XHCI_DEVICES; index++) {
        if (g_slot_pool[index].used && g_slot_pool[index].slot == slot) {
            return &g_slot_pool[index];
        }
    }

    return NULL;
}

/* The place of the pool for a slot the controller just enabled. */
static struct xhci_slot_state *slot_take(k_u32_t slot)
{
    k_u32_t index;

    for (index = 0; index < XHCI_DEVICES; index++) {
        if (!g_slot_pool[index].used) {
            memset(&g_slot_pool[index], 0, sizeof(g_slot_pool[index]));
            g_slot_pool[index].used = true;
            g_slot_pool[index].slot = (k_u8_t)slot;
            g_slot_pool[index].ep0_cycle = 1u;
            return &g_slot_pool[index];
        }
    }

    return NULL;
}

static void slot_release(k_u32_t slot)
{
    struct xhci_slot_state *state = slot_state_of(slot);

    if (state != NULL) {
        state->used = false;
    }
}

/* The doorbell of a slot: the endpoint is the index of the context of the
 * endpoint (1 for the endpoint 0, 3 for the endpoint 1 IN), and the doorbell
 * 0 is the command ring. */
static void doorbell(k_u32_t slot, k_u32_t endpoint)
{
    *(volatile k_u32_t *)(g_db + slot * 4u) = endpoint;
}

/* The memory a device reads is the memory of the machine, and the image of
 * the kernel is the only place below the top 2 GiB whose two addresses are
 * not the same number (ADR 0032). */
static k_u64_t trb_phys(const void *trb)
{
    return port_x86_64_bus_phys(trb);
}

/*
 * A wait of the driver. The clock of the machine is the PIT, a millisecond a
 * tick, and the interrupts are on when the driver runs, so the deadline moves
 * by itself; the count is a guard for a machine whose clock is not there
 * (the boot arms this driver after the timer, and a port whose timer broke
 * has a keyboard that does not come up and not a machine that hangs).
 */
static bool wait_until(k_u32_t offset, k_u32_t mask, k_u32_t value, k_u32_t timeout_ms,
                       k_u32_t spins)
{
    k_u64_t deadline = k_port_uptime_ms() + timeout_ms;
    k_u32_t guard;

    for (guard = 0; guard < spins; guard++) {
        if ((op_read(offset) & mask) == value) {
            return true;
        }

        if (k_port_uptime_ms() > deadline) {
            return false;
        }
    }

    return (op_read(offset) & mask) == value;
}

/*
 * The clock of the waits of this file, in microseconds. The tick of the port
 * is a millisecond and the reset of a port is decided in tens of them; the
 * time stamp counter is the clock the boot calibrated against the PIT, and it
 * is what a deadline of a reset is measured with. A machine whose boot could
 * not calibrate it keeps the tick, which is coarse and true.
 */
static k_u64_t now_us(void)
{
    const struct port_x86_64_info *info = port_x86_64_info();

    if (info != NULL && info->tsc_calibrated && info->cpu_hz != 0u) {
        k_u64_t cycles = port_rdtsc();
        k_u64_t hz = info->cpu_hz;

        return (cycles / hz) * 1000000u + ((cycles % hz) * 1000000u) / hz;
    }

    return k_port_uptime_us();
}

/* Spend a few milliseconds without a ring to wait on. The clock of the
 * machine is the tick of the PIT, so a wait is a deadline and the loop reads
 * the register of the controller between two of them - which is also the
 * moment the device behind the port needs to be ready. */
static void delay_ms(k_u32_t milliseconds)
{
    k_u64_t deadline = k_port_uptime_ms() + milliseconds;

    while (k_port_uptime_ms() < deadline) {
        __asm__ volatile("pause");
    }
}

/* --- the event ring -------------------------------------------------------- */

/* The event the controller wrote, or NULL when the ring is empty. The cycle
 * bit says which of the two halves of a wrapped ring the entry belongs to. */
static struct xhci_trb *event_peek(void)
{
    struct xhci_trb *trb = &g_event_ring[g_evt_index];

    if ((trb->control & XHCI_TRB_CYCLE) != g_evt_cycle) {
        return NULL;
    }

    return trb;
}

/* Give one event back: the dequeue pointer of the machine moves and the
 * event handler busy bit is cleared with it (EHB, the bit 3). */
static void event_pop(void)
{
    g_evt_index++;

    if (g_evt_index == XHCI_EVENT_TRBS) {
        g_evt_index = 0;
        g_evt_cycle ^= 1u;
    }

    rt_write64(XHCI_RT_ERDP, trb_phys(&g_event_ring[g_evt_index]) | 0x8u);
}

/*
 * The ring is one queue for every consumer of the controller, and a consumer
 * cannot skip an event of another one: the head of the ring is the order. The
 * pump below is the only reader: it decodes the reports of the keyboard and
 * of the mouse itself, and leaves a transfer event of a class and the
 * completion of a command in two small queues of their own, where the call
 * that waits for one finds it. Without that, a console poll in the middle of
 * a frame of the network would take the completion of the frame and the wait
 * would time out.
 */
#define XHCI_XFER_QUEUE 16u
#define XHCI_CMD_QUEUE 8u

static struct xhci_trb g_xfer_queue[XHCI_XFER_QUEUE];
static k_u32_t g_xfer_head;
static k_u32_t g_xfer_tail;
static struct xhci_trb g_cmd_queue[XHCI_CMD_QUEUE];
static k_u32_t g_cmd_head;
static k_u32_t g_cmd_tail;

static void queue_put(struct xhci_trb *queue, k_u32_t *head, k_u32_t *tail, k_u32_t size,
                      const struct xhci_trb *event)
{
    k_u32_t next = (*head + 1u) % size;

    if (next == *tail) {
        /* A queue that is full loses its oldest event: a completion nobody
         * has read for a whole transfer of the driver is a wait that has
         * already timed out. */
        *tail = (*tail + 1u) % size;
    }

    queue[*head] = *event;
    *head = next;
}

/* Take one event of a queue that the caller names, or false when it is not
 * there yet. The order of the queue is kept for the consumers behind. */
static bool queue_take(struct xhci_trb *queue, k_u32_t *head, k_u32_t *tail, k_u32_t size,
                       bool (*match)(const struct xhci_trb *event, const void *wanted),
                       const void *wanted, struct xhci_trb *out)
{
    k_u32_t index = *tail;
    k_u32_t position = *tail;

    while (index != *head) {
        if (match(&queue[index], wanted)) {
            *out = queue[index];

            for (position = index; (position + 1u) % size != *head;
                 position = (position + 1u) % size) {
                queue[position] = queue[(position + 1u) % size];
            }

            *head = position;
            return true;
        }

        index = (index + 1u) % size;
    }

    return false;
}

static void usb_service(void);

static bool match_command(const struct xhci_trb *event, const void *wanted)
{
    return XHCI_TRB_GET_TYPE(event->control) == XHCI_TRB_COMMAND_COMPLETION &&
           event->param == *(const k_u64_t *)wanted;
}

struct xhci_transfer_wanted {
    k_u32_t slot;
    k_u32_t dci;
};

static bool match_transfer(const struct xhci_trb *event, const void *wanted)
{
    const struct xhci_transfer_wanted *id = (const struct xhci_transfer_wanted *)wanted;

    return XHCI_TRB_GET_TYPE(event->control) == XHCI_TRB_TRANSFER &&
           XHCI_TRB_SLOT(event->control) == id->slot &&
           XHCI_TRB_ENDPOINT(event->control) == id->dci;
}

/* --- the command ring ------------------------------------------------------ */

/* The last entry of a ring is a link to its first: the controller follows it
 * and the cycle bit of the ring turns over there. */
static void ring_link(struct xhci_trb *ring, k_u32_t count, k_u32_t cycle)
{
    struct xhci_trb *link = &ring[count - 1u];

    link->param = trb_phys(ring);
    link->status = 0;
    link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TC | cycle;
}

/* Put one TRB on the command ring and answer its address, which is what a
 * command completion event names. */
static struct xhci_trb *command_put(const struct xhci_trb *trb)
{
    struct xhci_trb *slot;

    if (g_cmd_index == XHCI_CMD_TRBS - 1u) {
        ring_link(g_cmd_ring, XHCI_CMD_TRBS, g_cmd_cycle);
        g_cmd_cycle ^= 1u;
        g_cmd_index = 0;
    }

    slot = &g_cmd_ring[g_cmd_index];
    *slot = *trb;
    slot->control |= g_cmd_cycle;
    g_cmd_index++;

    return slot;
}

/*
 * A command and its completion. The event names the TRB that asked, so a
 * completion of another command is not the answer of this one and is
 * dropped (the driver sends one command at a time and never has two).
 */
static bool command_wait(const struct xhci_trb *trb, k_u8_t *code, k_u32_t *slot_out)
{
    k_u64_t address = trb_phys(trb);
    k_u64_t deadline = k_port_uptime_ms() + 500u;

    for (;;) {
        struct xhci_trb event;

        usb_service();

        if (queue_take(g_cmd_queue, &g_cmd_head, &g_cmd_tail, XHCI_CMD_QUEUE, match_command,
                       &address, &event)) {
            *code = (k_u8_t)XHCI_TRB_CODE(event.status);
            *slot_out = XHCI_TRB_SLOT(event.control);
            return true;
        }

        if (k_port_uptime_ms() > deadline) {
            *code = 0xFFu;
            return false;
        }
    }
}

/* A command with no payload: the enable of a slot, the disable of it. The
 * slot of an enable is zero and the answer of the controller, and the slot
 * of a disable is the one given to the device. */
static bool command_simple(k_u32_t type, k_u32_t slot, k_u8_t *code, k_u32_t *slot_out)
{
    struct xhci_trb trb;
    struct xhci_trb *posted;

    trb.param = 0;
    trb.status = 0;
    trb.control = XHCI_TRB_TYPE(type) | (slot << 24);

    /* The compiler may not move the write of the ring behind the doorbell:
     * the controller reads the memory the moment it is told. */
    __asm__ volatile("" : : : "memory");
    posted = command_put(&trb);
    __asm__ volatile("" : : : "memory");
    doorbell(0, 0);

    return command_wait(posted, code, slot_out);
}

/* --- the context of a device and its endpoint 0 ---------------------------- */

static void context_put32(k_u8_t *context, k_u32_t offset, k_u32_t value)
{
    k_u32_t *word = (k_u32_t *)(void *)(context + offset);

    *word = value;
}

/* The slot context of an input context: the route from the root port through
 * the hubs above the device, the speed, the root port and how many endpoint
 * contexts follow it (1 for the endpoint 0, the highest index for a device
 * with an endpoint 1 IN). A hub says how many ports it has and marks itself
 * as a hub; the fields of the transaction translator are zero, because this
 * driver speaks to the devices of a hub of the full speed and not to a
 * full-speed one behind a high-speed hub (docs/debts.md). */
static void slot_context(k_u8_t *input, k_u32_t speed, const struct xhci_where *where,
                         k_u32_t entries, k_u32_t hub_ports)
{
    context_put32(input, 32u,
                  (entries << 27) | ((hub_ports != 0u ? 1u : 0u) << 26) | (speed << 20) |
                      (where->route & 0xFFFFFu));
    context_put32(input, 36u, (hub_ports << 24) | ((k_u32_t)where->root_port << 16));
}

/* One endpoint context: the interval, the error count, the type and the
 * largest packet of the endpoint, and the ring it reads. */
static void endpoint_context(k_u8_t *input, k_u32_t dci, k_u32_t type, k_u32_t interval,
                             k_u32_t max_packet, const void *ring)
{
    k_u32_t offset = 32u + 32u * dci;
    k_u64_t address = trb_phys(ring);

    context_put32(input, offset + 0u, interval << 16);
    context_put32(input, offset + 4u, (3u << 1) | (type << 3) | (max_packet << 16));
    context_put32(input, offset + 8u, ((k_u32_t)address & 0xFFFFFFF0u) | 1u);
    context_put32(input, offset + 12u, (k_u32_t)(address >> 32));
    context_put32(input, offset + 16u, 8u);
}

/* The largest packet of the endpoint 0 of a device of this speed, before the
 * descriptor of the device says its own. */
static k_u32_t ep0_packet(k_u32_t speed)
{
    if (speed == XHCI_SPEED_HIGH) {
        return 64u;
    }

    if (speed == XHCI_SPEED_SUPER) {
        return 512u;
    }

    return 8u;
}

static k_u32_t slot_index_of(k_u32_t slot)
{
    struct xhci_slot_state *state = slot_state_of(slot);

    return state != NULL ? (k_u32_t)(state - g_slot_pool) : XHCI_DEVICES;
}

/* The address of the device: the slot is given its context and the ring of
 * its endpoint 0, and the controller sends SET_ADDRESS to it. The route and
 * the root port are the way the controller finds a device behind a hub. */
static bool address_device(k_u32_t slot, const struct xhci_where *where, k_u32_t speed,
                           k_u32_t packet, k_u32_t hub_ports)
{
    struct xhci_trb trb;
    k_u8_t code = 0;
    k_u32_t answer = 0;
    k_u32_t index = slot_index_of(slot);

    if (index >= XHCI_DEVICES) {
        return false;
    }

    memset(g_input, 0, sizeof(g_input));
    context_put32(g_input, 0u, 0u);                    /* the contexts to drop */
    context_put32(g_input, 4u, (1u << 0) | (1u << 1)); /* the slot and the endpoint 0 */
    slot_context(g_input, speed, where, 1u, hub_ports);
    endpoint_context(g_input, 1u, XHCI_EP_TYPE_CONTROL, 0u, packet, g_ep0_ring[index]);

    trb.param = trb_phys(g_input);
    trb.status = 0;
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) | (slot << 24);

    __asm__ volatile("" : : : "memory");
    {
        struct xhci_trb *posted = command_put(&trb);

        __asm__ volatile("" : : : "memory");
        doorbell(0, 0);

        if (!command_wait(posted, &code, &answer)) {
            kwarn("xhci: the slot %u did not answer the address", (unsigned)slot);
            return false;
        }
    }

    if (code != XHCI_CODE_SUCCESS) {
        kwarn("xhci: the device of the slot %u was not addressed (%u)", (unsigned)slot,
              (unsigned)code);
        return false;
    }

    /* The recovery time of SET_ADDRESS (USB 2.0, 9.2.6.3): two milliseconds
     * before the first request of the descriptor. */
    delay_ms(2u);

    return true;
}

/* The context of the endpoint 0 with a larger packet: a device of the full
 * speed says its own in the first eight bytes of its descriptor. */
static bool evaluate_ep0(k_u32_t slot, const struct xhci_where *where, k_u32_t speed,
                         k_u32_t packet)
{
    struct xhci_trb trb;
    k_u8_t code = 0;
    k_u32_t answer = 0;
    k_u32_t index = slot_index_of(slot);

    if (index >= XHCI_DEVICES) {
        return false;
    }

    /* The slot and the endpoint 0 are the two contexts of this command: the
     * add flags say which of them the controller reads. */
    memset(g_input, 0, sizeof(g_input));
    context_put32(g_input, 0u, 0u);
    context_put32(g_input, 4u, (1u << 0) | (1u << 1));
    slot_context(g_input, speed, where, 1u, 0u);
    endpoint_context(g_input, 1u, XHCI_EP_TYPE_CONTROL, 0u, packet, g_ep0_ring[index]);

    trb.param = trb_phys(g_input);
    trb.status = 0;
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CONTEXT) | (slot << 24);

    __asm__ volatile("" : : : "memory");
    {
        struct xhci_trb *posted = command_put(&trb);

        __asm__ volatile("" : : : "memory");
        doorbell(0, 0);

        if (!command_wait(posted, &code, &answer)) {
            return false;
        }
    }

    return code == XHCI_CODE_SUCCESS;
}

/* The hub is a hub to the controller and not only to the driver: the slot
 * context says how many ports it has, and the command that changes it is the
 * one of a context that is already addressed. */
static bool evaluate_hub(k_u32_t slot, const struct xhci_where *where, k_u32_t speed,
                         k_u32_t packet, k_u32_t ports)
{
    struct xhci_trb trb;
    k_u8_t code = 0;
    k_u32_t answer = 0;
    k_u32_t index = slot_index_of(slot);

    if (index >= XHCI_DEVICES) {
        return false;
    }

    memset(g_input, 0, sizeof(g_input));
    context_put32(g_input, 0u, 0u);
    context_put32(g_input, 4u, (1u << 0) | (1u << 1));
    slot_context(g_input, speed, where, 1u, ports);
    endpoint_context(g_input, 1u, XHCI_EP_TYPE_CONTROL, 0u, packet, g_ep0_ring[index]);

    trb.param = trb_phys(g_input);
    trb.status = 0;
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CONTEXT) | (slot << 24);

    __asm__ volatile("" : : : "memory");
    {
        struct xhci_trb *posted = command_put(&trb);

        __asm__ volatile("" : : : "memory");
        doorbell(0, 0);

        if (!command_wait(posted, &code, &answer)) {
            return false;
        }
    }

    return code == XHCI_CODE_SUCCESS;
}

/* One endpoint of the reports, of the keyboard or of the mouse: its context,
 * its ring, and the device configured with it. The ring of the caller is
 * cleared and linked before the command, because a slot the controller has
 * seen before may still hold the dequeue of a device that left. */
static bool configure_interrupt(k_u32_t slot, const struct xhci_where *where, k_u32_t speed,
                                k_u32_t dci, k_u32_t interval, k_u32_t max_packet,
                                struct xhci_trb *ring, k_u32_t *index_out, k_u32_t *cycle_out)
{
    struct xhci_trb trb;
    k_u8_t code = 0;
    k_u32_t answer = 0;

    *index_out = 0;
    *cycle_out = 1u;
    memset(ring, 0, XHCI_EP_TRBS * sizeof(*ring));
    ring_link(ring, XHCI_EP_TRBS, *cycle_out);

    memset(g_input, 0, sizeof(g_input));
    context_put32(g_input, 0u, 0u);
    context_put32(g_input, 4u, (1u << 0) | (1u << dci));
    slot_context(g_input, speed, where, dci, 0u);
    endpoint_context(g_input, dci, XHCI_EP_TYPE_INTERRUPT_IN, interval, max_packet, ring);

    trb.param = trb_phys(g_input);
    trb.status = 0;
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) | (slot << 24);

    __asm__ volatile("" : : : "memory");
    {
        struct xhci_trb *posted = command_put(&trb);

        __asm__ volatile("" : : : "memory");
        doorbell(0, 0);

        if (!command_wait(posted, &code, &answer)) {
            return false;
        }
    }

    return code == XHCI_CODE_SUCCESS;
}

/* --- the endpoint 0: a control transfer ------------------------------------ */

static struct xhci_trb *ep0_put(k_u32_t index, const struct xhci_trb *trb)
{
    struct xhci_trb *slot;
    struct xhci_slot_state *state;

    if (index >= XHCI_DEVICES) {
        return NULL;
    }

    state = &g_slot_pool[index];

    if (state->ep0_index == XHCI_EP_TRBS - 1u) {
        ring_link(g_ep0_ring[index], XHCI_EP_TRBS, state->ep0_cycle);
        state->ep0_cycle ^= 1u;
        state->ep0_index = 0;
    }

    slot = &g_ep0_ring[index][state->ep0_index];
    *slot = *trb;
    slot->control |= state->ep0_cycle;
    state->ep0_index++;

    return slot;
}

/* The answer of one endpoint: a transfer event of this slot and this
 * endpoint, whose completion code says how the transfer ended and whose
 * length says how many bytes the device did not move. */
static bool wait_transfer(k_u32_t slot, k_u32_t dci, k_u32_t timeout_ms, k_u8_t *code,
                          k_u32_t *left)
{
    k_u64_t deadline = k_port_uptime_ms() + timeout_ms;
    struct xhci_transfer_wanted wanted;

    wanted.slot = slot;
    wanted.dci = dci;

    for (;;) {
        struct xhci_trb event;

        usb_service();

        if (queue_take(g_xfer_queue, &g_xfer_head, &g_xfer_tail, XHCI_XFER_QUEUE, match_transfer,
                       &wanted, &event)) {
            *code = (k_u8_t)XHCI_TRB_CODE(event.status);
            *left = XHCI_TRB_LENGTH(event.status);
            return true;
        }

        if (k_port_uptime_ms() > deadline) {
            *code = 0xFFu;
            return false;
        }
    }
}

/*
 * One transfer of the endpoint 0: the setup stage, the data stage when the
 * request has data, and the status stage, which is the chain of TRBs the
 * specification calls a transfer descriptor. A request that moves no data is
 * two TRBs, and the status stage of a request of the device is the other
 * direction of its data.
 */
static bool control_transfer(k_u32_t slot, k_u8_t request_type, k_u8_t request, k_u16_t value,
                             k_u16_t index, k_u16_t length, void *data)
{
    struct xhci_trb trb;
    k_u8_t setup[8];
    k_u64_t word = 0;
    k_u32_t ring_index = slot_index_of(slot);
    bool in = (request_type & 0x80u) != 0u;
    k_u8_t code = 0;
    k_u32_t left = 0;

    if (ring_index >= XHCI_DEVICES) {
        return false;
    }

    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (k_u8_t)(value & 0xFFu);
    setup[3] = (k_u8_t)(value >> 8);
    setup[4] = (k_u8_t)(index & 0xFFu);
    setup[5] = (k_u8_t)(index >> 8);
    setup[6] = (k_u8_t)(length & 0xFFu);
    setup[7] = (k_u8_t)(length >> 8);
    memcpy(&word, setup, sizeof(setup));

    __asm__ volatile("" : : : "memory");

    trb.param = word;
    trb.status = sizeof(setup);
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_SETUP) | XHCI_TRB_IDT | XHCI_TRB_CHAIN |
                  ((length == 0u ? 0u : (in ? 3u : 2u)) << 16);
    (void)ep0_put(ring_index, &trb);

    if (length != 0u) {
        trb.param = trb_phys(data);
        trb.status = length;
        trb.control = XHCI_TRB_TYPE(XHCI_TRB_DATA) | XHCI_TRB_CHAIN | (in ? XHCI_TRB_DIR_IN : 0u);
        (void)ep0_put(ring_index, &trb);
    }

    /* The status stage is the last TRB of the descriptor and the one that
     * raises the event: its direction is the one the data stage did not
     * take. */
    trb.param = 0;
    trb.status = 0;
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_STATUS) | XHCI_TRB_IOC |
                  ((length != 0u && in) ? 0u : XHCI_TRB_DIR_IN);
    (void)ep0_put(ring_index, &trb);

    __asm__ volatile("" : : : "memory");
    doorbell(slot, 1);

    if (!wait_transfer(slot, 1u, 500u, &code, &left)) {
        return false;
    }

    return code == XHCI_CODE_SUCCESS || code == XHCI_CODE_SHORT;
}

/* --- the configuration descriptor ----------------------------------------- */

struct usb_hid_interface {
    k_u8_t interface;
    k_u8_t endpoint;
    k_u16_t max_packet;
    k_u8_t interval;
    bool found;
};

/* The interface of a HID of the boot protocol and its endpoint of the
 * reports: class 3 (HID), sub-class 1, the protocol of the device (1 for a
 * keyboard, 2 for a mouse) and an interrupt endpoint of the device to the
 * host. A configuration with two such interfaces - a keyboard and a mouse in
 * one device - answers the one asked for. */
static void parse_hid(const k_u8_t *descriptor, k_u32_t length, k_u8_t protocol,
                      struct usb_hid_interface *hid)
{
    k_u32_t offset = 0;
    bool in_hid = false;

    hid->found = false;

    while (offset + 2u <= length) {
        k_u8_t size = descriptor[offset];
        k_u8_t type = descriptor[offset + 1u];

        if (size < 2u || offset + size > length) {
            return;
        }

        if (type == 4u && size >= 9u) {
            in_hid = descriptor[offset + 5u] == 3u && descriptor[offset + 6u] == 1u &&
                     descriptor[offset + 7u] == protocol;

            if (in_hid) {
                hid->interface = descriptor[offset + 2u];
            }
        } else if (type == 5u && size >= 7u && in_hid) {
            k_u8_t address = descriptor[offset + 2u];
            k_u8_t attributes = descriptor[offset + 3u];

            if ((address & 0x80u) != 0u && (attributes & 0x03u) == 0x03u) {
                hid->endpoint = address;
                hid->max_packet =
                    (k_u16_t)(descriptor[offset + 4u] | ((k_u16_t)descriptor[offset + 5u] << 8));
                hid->interval = descriptor[offset + 6u];
                hid->found = true;
                return;
            }
        }

        offset += size;
    }
}

/* The index of the context of an endpoint from its address: the endpoint 0 is
 * 1, the OUT of the endpoint n is 2n and its IN is 2n + 1. */
static k_u32_t endpoint_dci(k_u8_t address)
{
    k_u32_t number = address & 0x0Fu;

    return (address & 0x80u) != 0u ? 2u * number + 1u : 2u * number;
}

/*
 * The interval of an endpoint as the context wants it. The field of the
 * context is not the bInterval of the descriptor: a full-speed endpoint
 * counts frames, and the controller of a high-speed or a SuperSpeed endpoint
 * counts the power of two - the descriptor says 7 for a keyboard of 8 ms and
 * the context says 6, because the period is 2^6 microframes of 125 us.
 */
static k_u32_t endpoint_interval(k_u8_t interval, k_u32_t speed)
{
    if (interval == 0u) {
        return 1u;
    }

    if (speed < XHCI_SPEED_HIGH) {
        return interval;
    }

    return interval > 16u ? 15u : (k_u32_t)(interval - 1u);
}

/* --- the classes on top of the controller --------------------------------- */

/*
 * A device of a class is a slot, the endpoints the class asked for, and the
 * rings those endpoints read. The class drivers (`src/usb_storage.c` and
 * `src/usb_net.c`) know the descriptors and the commands of their class and
 * call this file for a transfer; the rings, the contexts, the doorbells and
 * the physical addresses stay here.
 *
 * The pool is eight devices, as the pool of the slots is: a machine of the
 * release has a keyboard, a mouse, a stick, a wire and the hubs between them.
 * The rings are sixteen TRBs and a bulk transfer of this driver is at most
 * fifteen of them, so a transfer never wraps the ring between its first and
 * its last TRB.
 */
#define XHCI_DEV_EP 4u
#define XHCI_DEV_EP_RING 16u
#define XHCI_DEV_EP_PUT (XHCI_DEV_EP_RING - 1u)
#define XHCI_PAGE 4096u

enum xhci_usb_class {
    XHCI_USB_NONE = 0,
    XHCI_USB_KEYBOARD,
    XHCI_USB_MOUSE,
    XHCI_USB_STORAGE,
    XHCI_USB_NET,
    XHCI_USB_HUB,
};

struct xhci_endpoint_state {
    bool used;
    k_u8_t dci;
    k_u32_t type; /* the type of the endpoint context: 2, 6 or 7 */
    k_u16_t max_packet;
    k_u8_t interval;
    struct xhci_trb *ring;
    k_u32_t index; /* the producer of the ring */
    k_u32_t cycle;
};

struct xhci_class_device {
    bool used;
    k_u8_t slot;
    k_u8_t port;
    enum xhci_usb_class kind;
    struct xhci_where where;
    k_u32_t speed;
    k_u16_t vendor;
    k_u16_t product;
    k_u8_t config;
    struct xhci_endpoint_state eps[XHCI_DEV_EP];
    k_u32_t endpoints;
    struct port_x86_64_usb_device view;
};

static struct xhci_class_device g_class_device[XHCI_DEVICES];
static struct xhci_trb g_class_ring[XHCI_DEVICES][XHCI_DEV_EP][XHCI_DEV_EP_RING]
    __attribute__((aligned(64)));
static k_u8_t g_config[512] __attribute__((aligned(64)));

/* The device of a slot, or NULL when the class pool does not hold it. */
static struct xhci_class_device *class_device_of(k_u32_t slot)
{
    k_u32_t index;

    for (index = 0; index < XHCI_DEVICES; index++) {
        if (g_class_device[index].used && g_class_device[index].slot == slot) {
            return &g_class_device[index];
        }
    }

    return NULL;
}

/* The endpoint of a context index, or NULL when it is not on. */
static struct xhci_endpoint_state *class_endpoint(struct xhci_class_device *device, k_u32_t dci)
{
    k_u32_t index;

    if (device == NULL || dci == 0u) {
        return NULL;
    }

    for (index = 0; index < XHCI_DEV_EP; index++) {
        if (device->eps[index].used && device->eps[index].dci == dci) {
            return &device->eps[index];
        }
    }

    return NULL;
}

/*
 * Which class the interfaces of a configuration carry, in the order the
 * devices of this release are wanted: a keyboard and a mouse of the boot
 * protocol of HID, a hub, a stick of the Bulk-Only Transport, a wire of
 * CDC-ECM. A device of another class is enumerated and gives its slot back.
 */
static enum xhci_usb_class classify_configuration(const k_u8_t *descriptor, k_u32_t length)
{
    k_u32_t offset = 0;

    while (offset + 2u <= length) {
        k_u8_t size = descriptor[offset];
        k_u8_t type = descriptor[offset + 1u];

        if (size < 2u || offset + size > length) {
            break;
        }

        if (type == 4u && size >= 9u) {
            k_u8_t base = descriptor[offset + 5u];
            k_u8_t sub = descriptor[offset + 6u];
            k_u8_t protocol = descriptor[offset + 7u];

            if (base == 3u && sub == 1u && protocol == 1u) {
                return XHCI_USB_KEYBOARD;
            }

            if (base == 3u && sub == 1u && protocol == 2u) {
                return XHCI_USB_MOUSE;
            }

            if (base == 9u && sub == 0u) {
                return XHCI_USB_HUB;
            }

            if (base == 8u && sub == 6u && protocol == 0x50u) {
                return XHCI_USB_STORAGE;
            }

            if (base == 2u && sub == 6u) {
                return XHCI_USB_NET;
            }
        }

        offset += size;
    }

    return XHCI_USB_NONE;
}

/* Put one TRB on the ring of an endpoint, respecting the link at its end and
 * the cycle of the producer. */
static struct xhci_trb *endpoint_put(struct xhci_endpoint_state *endpoint,
                                     const struct xhci_trb *trb)
{
    struct xhci_trb *slot;

    if (endpoint->index >= XHCI_DEV_EP_PUT) {
        ring_link(endpoint->ring, XHCI_DEV_EP_RING, endpoint->cycle);
        endpoint->cycle ^= 1u;
        endpoint->index = 0;
    }

    slot = &endpoint->ring[endpoint->index];
    *slot = *trb;
    slot->control |= endpoint->cycle;
    endpoint->index++;

    return slot;
}

/*
 * The Configure Endpoint command of a class device: every endpoint that is
 * on is named again in the add flags, because the specification says the
 * controller forgets the contexts a command does not name.
 */
static bool class_configure(struct xhci_class_device *device)
{
    struct xhci_trb trb;
    struct xhci_trb *posted;
    k_u32_t add = 1u << 0;
    k_u32_t entries = 1u;
    k_u32_t index;
    k_u8_t code = 0;
    k_u32_t answer = 0;

    memset(g_input, 0, sizeof(g_input));

    for (index = 0; index < XHCI_DEV_EP; index++) {
        if (device->eps[index].used) {
            add |= 1u << device->eps[index].dci;

            if (device->eps[index].dci > entries) {
                entries = device->eps[index].dci;
            }
        }
    }

    context_put32(g_input, 0u, 0u);
    context_put32(g_input, 4u, add);
    slot_context(g_input, device->speed, &device->where, entries, 0u);

    for (index = 0; index < XHCI_DEV_EP; index++) {
        if (device->eps[index].used) {
            endpoint_context(g_input, device->eps[index].dci, device->eps[index].type,
                             device->eps[index].interval, device->eps[index].max_packet,
                             device->eps[index].ring);
        }
    }

    trb.param = trb_phys(g_input);
    trb.status = 0;
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) | (device->slot << 24);

    __asm__ volatile("" : : : "memory");
    posted = command_put(&trb);
    __asm__ volatile("" : : : "memory");
    doorbell(0, 0);

    if (!command_wait(posted, &code, &answer)) {
        return false;
    }

    return code == XHCI_CODE_SUCCESS;
}

/*
 * The physical address behind a byte of a transfer (ADR 0032 §1): the window
 * of the programs is the one memory of this machine whose virtual address is
 * not its physical one. Zero is a buffer the controller may not be given.
 */
static k_u64_t usb_phys(k_u64_t address)
{
    if (address >= PORT_X86_64_USER_BASE && address < PORT_X86_64_USER_LIMIT) {
        void *frame = port_x86_64_paging_phys(port_x86_64_current_cr3(),
                                              address & ~(k_u64_t)(XHCI_PAGE - 1u));

        if (frame == NULL) {
            return 0;
        }

        return (k_u64_t)(uintptr_t)frame | (address & (XHCI_PAGE - 1u));
    }

    return port_x86_64_bus_phys((const void *)(uintptr_t)address);
}

/* --- the calls a class driver makes --------------------------------------- */

static int endpoint_add(struct port_x86_64_usb_device *device, k_u8_t address, bool bulk,
                        k_u8_t interval, k_u16_t max_packet)
{
    struct xhci_class_device *class_device;
    struct xhci_endpoint_state *endpoint = NULL;
    k_u32_t dci;
    k_u32_t index;
    k_u32_t slot_index = XHCI_DEV_EP;
    k_u32_t device_index;

    if (device == NULL) {
        return -1;
    }

    class_device = class_device_of(device->slot);

    if (class_device == NULL) {
        return -1;
    }

    dci = endpoint_dci(address);

    for (index = 0; index < XHCI_DEV_EP; index++) {
        if (class_device->eps[index].used && class_device->eps[index].dci == dci) {
            endpoint = &class_device->eps[index];
            slot_index = index;
            break;
        }
    }

    if (endpoint == NULL) {
        for (index = 0; index < XHCI_DEV_EP; index++) {
            if (!class_device->eps[index].used) {
                endpoint = &class_device->eps[index];
                slot_index = index;
                break;
            }
        }
    }

    if (endpoint == NULL) {
        return -1;
    }

    device_index = (k_u32_t)(class_device - g_class_device);

    if (!endpoint->used) {
        memset(endpoint, 0, sizeof(*endpoint));
        endpoint->dci = (k_u8_t)dci;
        endpoint->ring = g_class_ring[device_index][slot_index];
        endpoint->cycle = 1u;
        memset(endpoint->ring, 0, sizeof(g_class_ring[device_index][slot_index]));
        ring_link(endpoint->ring, XHCI_DEV_EP_RING, endpoint->cycle);
        endpoint->used = true;
        class_device->endpoints++;
    }

    endpoint->type = bulk ? ((address & 0x80u) != 0u ? 6u : 2u) : 7u;
    endpoint->interval = interval;
    endpoint->max_packet = max_packet;

    if (!class_configure(class_device)) {
        endpoint->used = false;
        class_device->endpoints--;
        return -1;
    }

    if (bulk) {
        if ((address & 0x80u) != 0u) {
            device->bulk_in_dci = (k_u8_t)dci;
            device->bulk_in_packet = max_packet;
        } else {
            device->bulk_out_dci = (k_u8_t)dci;
            device->bulk_out_packet = max_packet;
        }
    }

    return 0;
}

int port_x86_64_xhci_endpoint(struct port_x86_64_usb_device *device, k_u8_t address, bool bulk,
                              k_u8_t interval, k_u16_t max_packet)
{
    int rc;

    if (device == NULL) {
        return -1;
    }

    if (k_mutex_lock(&g_xhci_lock) != 0) {
        return -1;
    }

    rc = endpoint_add(device, address, bulk, interval, max_packet);

    k_mutex_unlock(&g_xhci_lock);

    return rc;
}

int port_x86_64_xhci_control(struct port_x86_64_usb_device *device, k_u8_t request_type,
                             k_u8_t request, k_u16_t value, k_u16_t index, k_u16_t length,
                             void *data)
{
    int rc;

    if (device == NULL) {
        return -1;
    }

    if (k_mutex_lock(&g_xhci_lock) != 0) {
        return -1;
    }

    rc = control_transfer(device->slot, request_type, request, value, index, length, data) ? 0 : -1;

    k_mutex_unlock(&g_xhci_lock);

    return rc;
}

static int bulk_transfer(struct port_x86_64_usb_device *device, bool in, void *buffer,
                         k_u32_t length, k_u32_t timeout_ms, k_u32_t *moved)
{
    struct xhci_class_device *class_device;
    struct xhci_endpoint_state *endpoint;
    struct xhci_trb trb;
    k_u32_t done = 0;
    k_u8_t code = 0;
    k_u32_t left = 0;

    if (device == NULL || buffer == NULL || moved == NULL) {
        return -1;
    }

    *moved = 0;

    class_device = class_device_of(device->slot);
    endpoint = class_endpoint(class_device, in ? device->bulk_in_dci : device->bulk_out_dci);

    if (class_device == NULL || endpoint == NULL) {
        return -1;
    }

    __asm__ volatile("" : : : "memory");

    if (length == 0u) {
        /* A transfer of no bytes is a TRB of no bytes that still raises the
         * event: it is how a command with no data stage is asked. */
        trb.param = 0;
        trb.status = 0;
        trb.control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC;
        (void)endpoint_put(endpoint, &trb);
    }

    while (done < length) {
        k_u64_t address = (k_u64_t)(uintptr_t)((const k_u8_t *)buffer + done);
        k_size_t inside = (k_size_t)(address & (XHCI_PAGE - 1u));
        k_size_t piece = XHCI_PAGE - inside;
        k_u64_t physical;
        bool last;

        if (piece > (k_size_t)(length - done)) {
            piece = (k_size_t)(length - done);
        }

        physical = usb_phys(address);

        if (physical == 0u) {
            return -1;
        }

        last = done + piece >= length;
        trb.param = physical;
        trb.status = (k_u32_t)piece;
        trb.control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | (last ? XHCI_TRB_IOC : XHCI_TRB_CHAIN);
        (void)endpoint_put(endpoint, &trb);

        done += (k_u32_t)piece;
    }

    __asm__ volatile("" : : : "memory");
    doorbell(device->slot, endpoint->dci);

    if (!wait_transfer(device->slot, endpoint->dci, timeout_ms, &code, &left)) {
        return -1;
    }

    if (code != XHCI_CODE_SUCCESS && code != XHCI_CODE_SHORT) {
        return -1;
    }

    *moved = left <= length ? length - left : length;

    return 0;
}

int port_x86_64_xhci_bulk(struct port_x86_64_usb_device *device, bool in, void *buffer,
                          k_u32_t length, k_u32_t timeout_ms, k_u32_t *moved)
{
    int rc;

    if (device == NULL) {
        return -1;
    }

    if (k_mutex_lock(&g_xhci_lock) != 0) {
        return -1;
    }

    rc = bulk_transfer(device, in, buffer, length, timeout_ms, moved);

    k_mutex_unlock(&g_xhci_lock);

    return rc;
}

/* --- one device ------------------------------------------------------------ */

/* Give a slot back when the device is not the one this release wants. */
static void disable_slot(k_u32_t slot)
{
    struct xhci_trb trb;
    k_u8_t code = 0;
    k_u32_t answer = 0;

    trb.param = 0;
    trb.status = 0;
    trb.control = XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) | (slot << 24);

    __asm__ volatile("" : : : "memory");
    {
        struct xhci_trb *posted = command_put(&trb);

        __asm__ volatile("" : : : "memory");
        doorbell(0, 0);
        (void)command_wait(posted, &code, &answer);
    }

    g_dcbaa[slot] = 0;
    slot_release(slot);
}

/* The enumeration below reaches the ports of a hub and the ports reach the
 * enumeration: the two calls are one story and the forward names say so. */
static bool enumerate_device(const struct xhci_where *where, k_u32_t speed);
static k_u32_t detach_below(k_u32_t root_port, k_u32_t route, k_u32_t bits);
static void hub_scan_ports(struct xhci_hub *hub);

/*
 * The keyboard of the boot protocol: the configuration of the device is in
 * `g_config`, the slot is addressed, and its endpoint of the reports is the
 * one the class of the HID of the boot protocol names. The answer is true
 * when the keyboard is armed and the console can read its reports.
 */
static bool keyboard_attach(k_u32_t slot, const struct xhci_where *where, k_u32_t speed,
                            k_u16_t vendor, k_u16_t product, k_u32_t length)
{
    /* The parser fills the fields it finds and the caller reads the ones it
     * asked for; a compiler that cannot see the two in one pass wants the
     * whole of the record written first (GCC 11 of the cloud, -O2). */
    struct usb_hid_interface keyboard = {0};
    k_u32_t index;
    k_u32_t dci;
    k_u32_t interval;
    k_u32_t max_packet;

    parse_hid(g_config, length, 1u, &keyboard);

    if (!keyboard.found) {
        return false;
    }

    if (g_keyboard.present) {
        kinfo("usb: a second keyboard of the boot protocol on port %u is not used",
              (unsigned)where->root_port);
        return false;
    }

    /* SET_PROTOCOL (boot): a keyboard of the HID has a report of eight bytes
     * only in the boot protocol, and this driver reads the eight. */
    if (!control_transfer(slot, 0x21u, 0x0Bu, 0u, keyboard.interface, 0u, NULL)) {
        kwarn("usb: the keyboard did not take the boot protocol, its report is the one of the "
              "HID");
    }

    dci = endpoint_dci(keyboard.endpoint);
    interval = endpoint_interval(keyboard.interval, speed);
    max_packet = keyboard.max_packet & 0x07FFu;

    if (!configure_interrupt(slot, where, speed, dci, interval, max_packet, g_kbd_ring,
                             &g_keyboard.index, &g_keyboard.cycle)) {
        kinfo("usb: port %u: the endpoint of the reports did not come up",
              (unsigned)where->root_port);
        return false;
    }

    g_keyboard.present = true;
    g_keyboard.slot = (k_u8_t)slot;
    g_keyboard.dci = (k_u8_t)dci;
    g_keyboard.interface = keyboard.interface;
    g_keyboard.max_packet = (k_u16_t)max_packet;
    g_keyboard.where = *where;
    k_input_hid_init(&g_hid);

    /* Seven reports are in the ring of the endpoint before the first key
     * arrives: a person who types while the boot still prints a line does
     * not lose the first letter to the speed of the poll. */
    for (index = 0; index < XHCI_KBD_BUFFERS; index++) {
        k_u32_t cursor = g_keyboard.index;

        memset(g_kbd_report[cursor], 0, K_INPUT_HID_REPORT);
        {
            struct xhci_trb *trb = &g_kbd_ring[cursor];

            trb->param = trb_phys(g_kbd_report[cursor]);
            trb->status = K_INPUT_HID_REPORT;
            trb->control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | g_keyboard.cycle;
        }
        g_keyboard.index++;
    }

    __asm__ volatile("" : : : "memory");
    doorbell(slot, dci);

    kinfo("usb: keyboard %04x:%04x on port %u, slot %u, interface %u, %u bytes", (unsigned)vendor,
          (unsigned)product, (unsigned)where->root_port, (unsigned)slot,
          (unsigned)keyboard.interface, (unsigned)max_packet);

    return true;
}

/*
 * The mouse of the boot protocol: the same shape as the keyboard, with a
 * report of three bytes and the fourth of the wheel. The device is armed with
 * seven places, as the keyboard is, because a movement that finds no buffer
 * is a movement the device NAKs until the next poll.
 */
static bool mouse_attach(k_u32_t slot, const struct xhci_where *where, k_u32_t speed,
                         k_u16_t vendor, k_u16_t product, k_u32_t length)
{
    struct usb_hid_interface mouse;
    k_u32_t index;
    k_u32_t dci;
    k_u32_t interval;
    k_u32_t max_packet;

    parse_hid(g_config, length, 2u, &mouse);

    if (!mouse.found) {
        return false;
    }

    if (g_mouse.present) {
        kinfo("usb: a second mouse of the boot protocol on port %u is not used",
              (unsigned)where->root_port);
        return false;
    }

    if (!control_transfer(slot, 0x21u, 0x0Bu, 0u, mouse.interface, 0u, NULL)) {
        kwarn("usb: the mouse did not take the boot protocol, its report is the one of the HID");
    }

    dci = endpoint_dci(mouse.endpoint);
    interval = endpoint_interval(mouse.interval, speed);
    max_packet = mouse.max_packet & 0x07FFu;

    if (max_packet < 3u) {
        kinfo("usb: port %u: the mouse names an endpoint packet of %u byte(s)",
              (unsigned)where->root_port, (unsigned)max_packet);
        return false;
    }

    /* The endpoint of a mouse of the boot protocol carries a report of three
     * bytes, or four with a wheel, whatever the largest packet of the device
     * is: the transfer asks for the report and the device answers a short
     * packet when it has fewer bytes to give. The context keeps the packet of
     * the descriptor. */
    {
        k_u32_t report_length =
            max_packet >= K_INPUT_HID_MOUSE_REPORT ? K_INPUT_HID_MOUSE_REPORT : 3u;

        if (!configure_interrupt(slot, where, speed, dci, interval, max_packet, g_mouse_ring,
                                 &g_mouse.index, &g_mouse.cycle)) {
            kinfo("usb: port %u: the endpoint of the mouse did not come up",
                  (unsigned)where->root_port);
            return false;
        }

        g_mouse.present = true;
        g_mouse.slot = (k_u8_t)slot;
        g_mouse.dci = (k_u8_t)dci;
        g_mouse.interface = mouse.interface;
        g_mouse.max_packet = (k_u16_t)report_length;
        g_mouse.where = *where;
        k_input_hid_mouse_init(&g_mouse_hid);
    }

    for (index = 0; index < XHCI_KBD_BUFFERS; index++) {
        k_u32_t cursor = g_mouse.index;
        struct xhci_trb *trb = &g_mouse_ring[cursor];

        memset(g_mouse_report[cursor], 0, K_INPUT_HID_MOUSE_REPORT);
        trb->param = trb_phys(g_mouse_report[cursor]);
        trb->status = g_mouse.max_packet;
        trb->control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | g_mouse.cycle;
        g_mouse.index++;
    }

    __asm__ volatile("" : : : "memory");
    doorbell(slot, dci);

    kinfo("usb: mouse %04x:%04x on port %u, slot %u, interface %u, %u byte report",
          (unsigned)vendor, (unsigned)product, (unsigned)where->root_port, (unsigned)slot,
          (unsigned)mouse.interface, (unsigned)g_mouse.max_packet);

    return true;
}

/* --- the hub (class 09) ----------------------------------------------------- */

/* The requests of a hub (USB 2.0, 11.24 and USB 3.0, 10.16). */
#define USB_CLASS_HUB 9u
#define USB_DT_HUB 0x29u
#define HUB_PORT_POWER 8u
#define HUB_PORT_RESET 4u
#define HUB_CLEAR_CONNECTION 16u
#define HUB_CLEAR_ENABLE 17u
#define HUB_CLEAR_SUSPEND 18u
#define HUB_CLEAR_OVER_CURRENT 19u
#define HUB_CLEAR_RESET 20u

#define HUB_PORT_STAT_CONNECTION 0x0001u
#define HUB_PORT_STAT_ENABLE 0x0002u
#define HUB_PORT_STAT_RESET 0x0010u
#define HUB_PORT_STAT_POWER 0x0100u
#define HUB_PORT_STAT_LOW_SPEED 0x0200u
#define HUB_PORT_STAT_HIGH_SPEED 0x0400u

/* The change bits of wPortChange, which are not the selectors of the
 * features that clear them. */
#define HUB_CHANGE_CONNECTION 0x0001u
#define HUB_CHANGE_ENABLE 0x0002u
#define HUB_CHANGE_SUSPEND 0x0004u
#define HUB_CHANGE_OVER_CURRENT 0x0008u
#define HUB_CHANGE_RESET 0x0010u

/* The most ports of a hub this driver walks and the most tiers of them: the
 * route string has five nibbles and this release takes four of them. */
#define XHCI_HUB_PORTS 8u
#define XHCI_HUB_LEVELS 4u

/* The answer of GET_PORT_STATUS: the status and the change bits. */
static bool hub_status(struct xhci_hub *hub, k_u32_t port, k_u16_t *status, k_u16_t *change)
{
    k_u8_t data[4];

    if (!control_transfer(hub->slot, 0xA3u, 0u, 0u, (k_u16_t)port, sizeof(data), data)) {
        return false;
    }

    *status = (k_u16_t)(data[0] | ((k_u16_t)data[1] << 8));
    *change = (k_u16_t)(data[2] | ((k_u16_t)data[3] << 8));

    return true;
}

/* A feature of one port: SET_FEATURE and CLEAR_FEATURE of the hub class. */
static bool hub_set_port(struct xhci_hub *hub, k_u32_t port, k_u16_t feature, bool set)
{
    return control_transfer(hub->slot, 0x23u, set ? 3u : 1u, feature, (k_u16_t)port, 0u, NULL);
}

/* The speed of a port as the controller names it: the status of a hub of the
 * USB 2.0 has a bit for the low and one for the high speed, and the device of
 * a SuperSpeed hub is a SuperSpeed one whatever the field says. */
static k_u32_t hub_port_speed(struct xhci_hub *hub, k_u16_t status)
{
    if (hub->superspeed) {
        return XHCI_SPEED_SUPER;
    }

    if ((status & HUB_PORT_STAT_LOW_SPEED) != 0u) {
        return XHCI_SPEED_LOW;
    }

    if ((status & HUB_PORT_STAT_HIGH_SPEED) != 0u) {
        return XHCI_SPEED_HIGH;
    }

    return XHCI_SPEED_FULL;
}

/* The reset of a port of a hub: the class request, the wait of the change bit
 * and the clear of the two changes it leaves (the reset and the enable). */
static bool hub_port_reset(struct xhci_hub *hub, k_u32_t port)
{
    k_u64_t deadline;
    k_u16_t status = 0;
    k_u16_t change = 0;
    k_u32_t guard;

    if (!hub_set_port(hub, port, HUB_PORT_RESET, true)) {
        return false;
    }

    deadline = k_port_uptime_ms() + 500u;

    for (guard = 0; guard < 100000u; guard++) {
        if (!hub_status(hub, port, &status, &change)) {
            return false;
        }

        if ((change & HUB_CHANGE_RESET) != 0u || k_port_uptime_ms() > deadline) {
            break;
        }
    }

    (void)hub_set_port(hub, port, HUB_CLEAR_RESET, false);
    (void)hub_set_port(hub, port, HUB_CLEAR_ENABLE, false);

    if (!hub_status(hub, port, &status, &change)) {
        return false;
    }

    /* QEMU sets the enable of a port on the reset; a real hub does too, and a
     * port that did not enable is a device that does not speak yet. */
    if ((status & HUB_PORT_STAT_ENABLE) == 0u) {
        delay_ms(20u);
        (void)hub_status(hub, port, &status, &change);
    }

    return (status & HUB_PORT_STAT_CONNECTION) != 0u;
}

/* The change bits of a port are cleared one at a time: the write of a feature
 * of the hub class names it. A connection that arrives while the scan runs is
 * a change of the next scan, and the bits below are read before they are
 * cleared. */
static void hub_clear_changes(struct xhci_hub *hub, k_u32_t port, k_u16_t change)
{
    if ((change & HUB_CHANGE_CONNECTION) != 0u) {
        (void)hub_set_port(hub, port, HUB_CLEAR_CONNECTION, false);
    }

    if ((change & HUB_CHANGE_ENABLE) != 0u) {
        (void)hub_set_port(hub, port, HUB_CLEAR_ENABLE, false);
    }

    if ((change & HUB_CHANGE_SUSPEND) != 0u) {
        (void)hub_set_port(hub, port, HUB_CLEAR_SUSPEND, false);
    }

    if ((change & HUB_CHANGE_OVER_CURRENT) != 0u) {
        (void)hub_set_port(hub, port, HUB_CLEAR_OVER_CURRENT, false);
    }

    if ((change & HUB_CHANGE_RESET) != 0u) {
        (void)hub_set_port(hub, port, HUB_CLEAR_RESET, false);
    }
}

/* The route of a device on a port of this hub: the bits of the hubs above it
 * and the nibble of this port. */
static k_u32_t hub_child_route(const struct xhci_hub *hub, k_u32_t port)
{
    return hub->route | (port << (4u * hub->depth));
}

/* One port of a hub changed: a device arrived or left, or a reset finished.
 * The scan reads the status and the changes, clears the changes, and gives
 * the port to the enumeration or takes its devices away. */
static void hub_port_changed(struct xhci_hub *hub, k_u32_t port, k_u16_t status, k_u16_t change)
{
    k_u32_t route = hub_child_route(hub, port);
    bool connected = (status & HUB_PORT_STAT_CONNECTION) != 0u;

    hub_clear_changes(hub, port, change);

    if ((change & (HUB_CHANGE_CONNECTION | HUB_CHANGE_ENABLE | HUB_CHANGE_RESET)) == 0u) {
        return;
    }

    if (!connected) {
        if (detach_below(hub->root_port, route, 4u * (hub->depth + 1u)) != 0u) {
            kinfo("usb: the device on the port %u of the hub %u is gone", (unsigned)port,
                  (unsigned)hub->slot);
        }
        return;
    }

    if ((change & (HUB_CHANGE_CONNECTION | HUB_CHANGE_RESET)) == 0u) {
        return;
    }

    if (hub->depth + 1u >= XHCI_HUB_LEVELS) {
        kinfo("usb: the hub %u is at the deepest level of the route string", (unsigned)hub->slot);
        return;
    }

    if (!hub_port_reset(hub, port)) {
        kinfo("usb: the port %u of the hub %u did not come up after its reset", (unsigned)port,
              (unsigned)hub->slot);
        return;
    }

    {
        struct xhci_where child;

        child.root_port = hub->root_port;
        child.depth = (k_u8_t)(hub->depth + 1u);
        child.parent_slot = hub->slot;
        child.route = route;

        (void)hub_status(hub, port, &status, &change);
        (void)enumerate_device(&child, hub_port_speed(hub, status));
    }
}

/* Every port of a hub, once: the status is a control transfer and a hub with
 * eight ports is eight of them, which is why the scan is four times a second
 * and not at every poll. */
static void hub_scan_ports(struct xhci_hub *hub)
{
    k_u32_t port;

    for (port = 1; port <= (k_u32_t)hub->ports; port++) {
        k_u16_t status = 0;
        k_u16_t change = 0;

        if (!hub_status(hub, port, &status, &change)) {
            continue;
        }

        if (change != 0u) {
            hub_port_changed(hub, port, status, change);
        }
    }
}

/*
 * The attach of a hub: its descriptor names the ports, every port is powered,
 * and the ports that already carry a device are enumerated at once. The
 * controller is told that the slot is a hub and how many ports it has; the
 * ports are polled, because the interrupt endpoint of the hub is a second
 * consumer of the event ring for a change at the pace of a hand.
 */
static bool hub_attach(k_u32_t slot, const struct xhci_where *where, k_u32_t speed, k_u32_t packet,
                       k_u16_t vendor, k_u16_t product, k_u8_t protocol)
{
    struct xhci_hub *hub = NULL;
    k_u32_t index;
    k_u32_t port;
    k_u32_t ports;
    k_u32_t power_ms;

    for (index = 0; index < XHCI_HUBS; index++) {
        if (!g_hubs[index].used) {
            hub = &g_hubs[index];
            break;
        }
    }

    if (hub == NULL) {
        kinfo("usb: the pool of the hubs is full, the port %u is not used",
              (unsigned)where->root_port);
        return false;
    }

    if (!control_transfer(slot, 0xA0u, 6u, (k_u16_t)(USB_DT_HUB << 8), 0u, sizeof(g_hub_desc),
                          g_hub_desc)) {
        kinfo("usb: port %u: the hub did not answer its descriptor", (unsigned)where->root_port);
        return false;
    }

    ports = g_hub_desc[2] & 0x0Fu;

    if (ports == 0u) {
        kinfo("usb: port %u: the hub has no ports", (unsigned)where->root_port);
        return false;
    }

    if (ports > XHCI_HUB_PORTS) {
        ports = XHCI_HUB_PORTS;
    }

    memset(hub, 0, sizeof(*hub));
    hub->used = true;
    hub->slot = (k_u8_t)slot;
    hub->root_port = where->root_port;
    hub->route = where->route;
    hub->depth = where->depth;
    hub->parent_slot = where->parent_slot;
    hub->ports = (k_u8_t)ports;
    hub->superspeed = protocol >= 2u;

    /* The power of every port, and the time the hub asks for it: a port that
     * is not powered is a port whose device does not answer. */
    for (port = 1; port <= ports; port++) {
        (void)hub_set_port(hub, port, HUB_PORT_POWER, true);
    }

    power_ms = (k_u32_t)g_hub_desc[5] * 2u;

    if (power_ms < 10u) {
        power_ms = 10u;
    }

    delay_ms(power_ms);

    (void)evaluate_hub(slot, where, speed, packet, ports);

    kinfo("usb: hub %04x:%04x on port %u, slot %u, %u port(s), %u level(s)", (unsigned)vendor,
          (unsigned)product, (unsigned)where->root_port, (unsigned)slot, (unsigned)ports,
          (unsigned)(where->depth + 1u));

    /* The ports that were powered with a device already on them: the connect
     * change of the moment is read and cleared by the scan. */
    hub_scan_ports(hub);

    return true;
}

/* --- a device of a class ---------------------------------------------------- */

/*
 * A device of a class: the pool of the class devices takes it, the class
 * driver reads the configuration and turns its endpoints on, and the answer
 * of the driver is the answer of the enumeration. A device the class cannot
 * bring up gives its slot back.
 */
static bool class_attach(enum xhci_usb_class kind, k_u32_t slot, const struct xhci_where *where,
                         k_u32_t speed, k_u16_t vendor, k_u16_t product, k_u32_t length)
{
    struct port_x86_64_usb_config config;
    struct xhci_class_device *device = NULL;
    k_u32_t index;
    bool attached;

    for (index = 0; index < XHCI_DEVICES; index++) {
        if (!g_class_device[index].used) {
            device = &g_class_device[index];
            break;
        }
    }

    if (device == NULL) {
        kinfo("usb: the pool of the class devices is full, the port %u is not used",
              (unsigned)where->root_port);
        return false;
    }

    memset(device, 0, sizeof(*device));
    device->used = true;
    device->slot = (k_u8_t)slot;
    device->port = where->root_port;
    device->kind = kind;
    device->where = *where;
    device->speed = speed;
    device->vendor = vendor;
    device->product = product;
    device->config = g_config[5];

    device->view.slot = (k_u8_t)slot;
    device->view.port = where->root_port;
    device->view.speed = speed;
    device->view.vendor = vendor;
    device->view.product = product;
    device->view.config = g_config[5];

    config.bytes = g_config;
    config.length = length;
    config.value = g_config[5];

    attached = kind == XHCI_USB_STORAGE ? port_x86_64_usb_storage_attach(&device->view, &config)
                                        : port_x86_64_usb_net_attach(&device->view, &config);

    if (!attached) {
        device->used = false;
    }

    return attached;
}

/* --- the removal of a device ------------------------------------------------ */

static void class_detach(struct xhci_class_device *device)
{
    if (!device->used) {
        return;
    }

    if (device->kind == XHCI_USB_STORAGE) {
        port_x86_64_usb_storage_detach(&device->view);
    } else if (device->kind == XHCI_USB_NET) {
        port_x86_64_usb_net_detach(&device->view);
    }

    device->used = false;
}

/* A device left: the class forgets it, the rings of the keyboard and of the
 * mouse are shut, and the slot goes back to the controller. The slot of a hub
 * is given back after its children, which the caller has already taken. */
static void detach_slot(k_u32_t slot)
{
    k_u32_t index;

    if (g_keyboard.present && g_keyboard.slot == slot) {
        g_keyboard.present = false;
        kinfo("usb: the keyboard on the port %u is gone", (unsigned)g_keyboard.where.root_port);
    }

    if (g_mouse.present && g_mouse.slot == slot) {
        g_mouse.present = false;
        kinfo("usb: the mouse on the port %u is gone", (unsigned)g_mouse.where.root_port);
    }

    for (index = 0; index < XHCI_DEVICES; index++) {
        if (g_class_device[index].used && g_class_device[index].slot == slot) {
            class_detach(&g_class_device[index]);
        }
    }

    for (index = 0; index < XHCI_HUBS; index++) {
        if (g_hubs[index].used && g_hubs[index].slot == slot) {
            g_hubs[index].used = false;
        }
    }

    disable_slot(slot);
}

static bool below_matches(k_u32_t root_port, k_u32_t route, k_u32_t bits, k_u32_t device_route)
{
    k_u32_t mask;

    if (bits == 0u) {
        return true;
    }

    if (bits >= 32u) {
        return device_route == route;
    }

    mask = (1u << bits) - 1u;

    return (device_route & mask) == (route & mask);
}

/*
 * The devices of one port of a hub (or of a root port, where `bits` is zero):
 * every record whose route starts with the port is taken away, a hub before
 * the devices behind it or after them, because each slot is its own command.
 * A list is made first, since the walk of the records must not change under
 * the loop.
 */
static k_u32_t detach_below(k_u32_t root_port, k_u32_t route, k_u32_t bits)
{
    k_u8_t slots[XHCI_DEVICES + XHCI_HUBS + 2u];
    k_u32_t count = 0;
    k_u32_t index;

    if (g_keyboard.present && g_keyboard.where.root_port == root_port &&
        below_matches(root_port, route, bits, g_keyboard.where.route)) {
        slots[count++] = g_keyboard.slot;
    }

    if (g_mouse.present && g_mouse.where.root_port == root_port &&
        below_matches(root_port, route, bits, g_mouse.where.route)) {
        slots[count++] = g_mouse.slot;
    }

    for (index = 0; index < XHCI_DEVICES; index++) {
        if (g_class_device[index].used && g_class_device[index].port == root_port &&
            below_matches(root_port, route, bits, g_class_device[index].where.route)) {
            slots[count++] = g_class_device[index].slot;
        }
    }

    for (index = 0; index < XHCI_HUBS; index++) {
        if (g_hubs[index].used && g_hubs[index].root_port == root_port &&
            below_matches(root_port, route, bits, g_hubs[index].route)) {
            slots[count++] = g_hubs[index].slot;
        }
    }

    for (index = 0; index < count; index++) {
        detach_slot(slots[index]);
    }

    return count;
}

/*
 * The enumeration of one device: a slot, an address, the descriptor of the
 * device and the configuration that carries a class this release drives. A
 * device of the USB has one or more configurations and the first one is not
 * always the one a driver wants - the network device of QEMU carries RNDIS in
 * the first and CDC-ECM in the second - so every configuration is read until
 * one names a class of this release.
 *
 * The answer is true when the device was brought up: a keyboard or a mouse
 * that reports, a hub with its ports, a stick that is a block device, or a
 * wire that is a network device.
 */
static bool enumerate_device(const struct xhci_where *where, k_u32_t speed)
{
    enum xhci_usb_class kind = XHCI_USB_NONE;
    struct xhci_slot_state *state;
    k_u8_t code = 0;
    k_u32_t slot = 0;
    k_u32_t index;
    k_u32_t packet = ep0_packet(speed);
    k_u32_t total = 0;
    k_u32_t chosen = 0;
    k_u32_t configurations;
    k_u32_t configuration;
    k_u16_t vendor = 0;
    k_u16_t product = 0;
    k_u8_t protocol = 0;

    if (!command_simple(XHCI_TRB_ENABLE_SLOT, 0, &code, &slot) || code != XHCI_CODE_SUCCESS ||
        slot == 0u) {
        kwarn("xhci: the controller gave no slot for the port %u", (unsigned)where->root_port);
        return false;
    }

    state = slot_take(slot);

    if (state == NULL) {
        /* The pool of the contexts of this driver is small on purpose: eight
         * devices of a machine are a keyboard, a mouse, a stick, a wire and
         * the ports of the hubs that carry them. */
        kinfo("xhci: the slot %u is past the pool of the driver", (unsigned)slot);
        disable_slot(slot);
        return false;
    }

    index = (k_u32_t)(state - g_slot_pool);
    memset(g_device_ctx[index], 0, 2048u);
    g_dcbaa[slot] = trb_phys(g_device_ctx[index]);
    memset(g_ep0_ring[index], 0, sizeof(g_ep0_ring[index]));
    state->ep0_index = 0;
    state->ep0_cycle = 1u;
    ring_link(g_ep0_ring[index], XHCI_EP_TRBS, state->ep0_cycle);

    if (!address_device(slot, where, speed, packet, 0u)) {
        disable_slot(slot);
        return false;
    }

    /* The first eight bytes of the descriptor of the device: the largest
     * packet of its endpoint 0 is the seventh, and a full-speed device that
     * answers with sixteen or thirty-two needs a context of that size before
     * the rest of the descriptor can arrive without a babble. */
    if (!control_transfer(slot, 0x80u, 6u, 0x0100u, 0u, 8u, g_desc)) {
        kinfo("usb: port %u: the device did not answer its descriptor", (unsigned)where->root_port);
        disable_slot(slot);
        return false;
    }

    {
        k_u32_t device_packet = g_desc[7];

        if ((device_packet == 8u || device_packet == 16u || device_packet == 32u ||
             device_packet == 64u) &&
            device_packet != packet) {
            if (!evaluate_ep0(slot, where, speed, device_packet)) {
                kinfo("usb: port %u: the endpoint 0 did not take its size",
                      (unsigned)where->root_port);
                disable_slot(slot);
                return false;
            }

            packet = device_packet;
        }
    }

    if (!control_transfer(slot, 0x80u, 6u, 0x0100u, 0u, 18u, g_desc)) {
        kinfo("usb: port %u: the device has no full descriptor", (unsigned)where->root_port);
        disable_slot(slot);
        return false;
    }

    vendor = (k_u16_t)(g_desc[8] | ((k_u16_t)g_desc[9] << 8));
    product = (k_u16_t)(g_desc[10] | ((k_u16_t)g_desc[11] << 8));
    protocol = g_desc[6];
    configurations = g_desc[17];

    for (configuration = 0; configuration < configurations && configuration < 4u; configuration++) {
        if (!control_transfer(slot, 0x80u, 6u, (k_u16_t)(0x0200u | configuration), 0u, 9u,
                              g_desc)) {
            break;
        }

        total = (k_u32_t)g_desc[2] | ((k_u32_t)g_desc[3] << 8);

        if (total < 9u || total > sizeof(g_desc)) {
            continue;
        }

        if (total > 9u && !control_transfer(slot, 0x80u, 6u, (k_u16_t)(0x0200u | configuration), 0u,
                                            (k_u16_t)total, g_desc)) {
            continue;
        }

        kind = classify_configuration(g_desc, total);

        if (kind != XHCI_USB_NONE) {
            memcpy(g_config, g_desc, total);
            chosen = total;
            break;
        }
    }

    if (kind == XHCI_USB_NONE) {
        kinfo("usb: port %u: %04x:%04x is not a device of a class this release drives",
              (unsigned)where->root_port, (unsigned)vendor, (unsigned)product);
        disable_slot(slot);
        return false;
    }

    /* SET_CONFIGURATION: the configuration of the descriptor, with no
     * payload and the value at byte five. */
    if (!control_transfer(slot, 0x00u, 9u, g_config[5], 0u, 0u, NULL)) {
        kinfo("usb: port %u: the device refused its configuration", (unsigned)where->root_port);
        disable_slot(slot);
        return false;
    }

    if (kind == XHCI_USB_KEYBOARD && keyboard_attach(slot, where, speed, vendor, product, chosen)) {
        return true;
    }

    if (kind == XHCI_USB_MOUSE && mouse_attach(slot, where, speed, vendor, product, chosen)) {
        return true;
    }

    if (kind == XHCI_USB_HUB && hub_attach(slot, where, speed, packet, vendor, product, protocol)) {
        return true;
    }

    if ((kind == XHCI_USB_STORAGE || kind == XHCI_USB_NET) &&
        class_attach(kind, slot, where, speed, vendor, product, chosen)) {
        return true;
    }

    disable_slot(slot);

    return false;
}

/* --- the port of the controller -------------------------------------------- */

/* The name of a speed, for the lines of the log a person reads. */
static const char *speed_name(k_u32_t speed)
{
    switch (speed) {
    case XHCI_SPEED_FULL:
        return "full";
    case XHCI_SPEED_LOW:
        return "low";
    case XHCI_SPEED_HIGH:
        return "high";
    case XHCI_SPEED_SUPER:
        return "super";
    default:
        return "unknown";
    }
}

/*
 * The power of the port, before anything else: a port that is not powered is
 * a port whose reset is not a reset.
 *
 * This is the defect the laptop of the report showed. The write of PP is
 * posted, and the old code read the register back at once, saw zero and
 * wrote the reset with the zero in it - which turned the port off in the
 * same write that reset it, and no PED ever came. A controller with PPC
 * clear (HCCPARAMS1 bit 3) powers its ports itself and PP reads zero for
 * ever; there the write and the wait are the price of a bit that is not
 * there, and the reset goes on - a port with a device on it is powered on
 * such a machine by definition.
 */
static bool port_power(k_u32_t address, k_u32_t state)
{
    k_u64_t started;

    if (!g_ppc || (state & XHCI_PORT_PP) != 0u) {
        return true;
    }

    op_write(address, (state & ~XHCI_PORT_CHANGE) | XHCI_PORT_PP);
    (void)op_read(address);

    started = now_us();

    while ((op_read(address) & XHCI_PORT_PP) == 0u) {
        if (now_us() - started > (k_u64_t)XHCI_PORT_POWER_MS * 1000u) {
            kwarn("usb: the port at %x did not take its power (PORTSC %08x)", (unsigned)address,
                  (unsigned)op_read(address));
            return false;
        }
    }

    /* The rail is up: the device behind it has the twenty milliseconds of the
     * specification before it is reset (§4.19.1). */
    delay_ms(XHCI_PORT_POWER_MS);

    return true;
}

/*
 * One attempt of a reset, and the PORTSC of the moment it gave up. The order
 * is the one of the specification and of the driver of Linux:
 *
 *   1. the change bits of an earlier scan (or of the firmware) are cleared,
 *      because a PRC that is already set is not the end of this reset;
 *   2. PR is written with the bits the port keeps - its power, its speed and
 *      its link state - and the write is read back, because a posted write
 *      that has not landed yet reads as a port that is not resetting;
 *   3. the answer is PRC and not the clearing of PR: on a real controller PR
 *      clears before the device is on the bus of the port, which is exactly
 *      how this driver read PED too early on the laptop of the report. The
 *      budget is 500 ms of the specification;
 *   4. the change bit of the reset is cleared after the enable, and PED is
 *      waited for in a loop: on a USB2 port it can lag the change bit by a
 *      few reads, and a USB3 port is enabled when PLS is U0 and not when it
 *      is Polling.
 */
static bool port_reset_once(k_u32_t address, k_u32_t *final, k_u32_t *speed)
{
    k_u32_t state = op_read(address);
    k_u64_t started;
    k_u32_t neutral;

    op_write(address, state | XHCI_PORT_CHANGE);
    (void)op_read(address);

    state = op_read(address);

    if ((state & XHCI_PORT_CCS) == 0u) {
        *final = state;
        return false;
    }

    neutral = state & (XHCI_PORT_PP | XHCI_PORT_PLS_MASK | XHCI_PORT_SPEED_MASK);
    op_write(address, neutral | XHCI_PORT_PR);
    (void)op_read(address);

    started = now_us();

    for (;;) {
        state = op_read(address);

        if ((state & XHCI_PORT_PRC) != 0u) {
            break;
        }

        if (now_us() - started > XHCI_RESET_TIMEOUT_US) {
            *final = state;
            return false;
        }
    }

    /* The reset is over: the change bit of it goes back as a one, and the
     * port is asked for its enable. */
    op_write(address, (state & ~XHCI_PORT_CHANGE) | XHCI_PORT_PRC);
    state = op_read(address);

    {
        k_u32_t speed_now = (state & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;
        k_u64_t enabled = now_us();

        for (;;) {
            k_u32_t pls = (state & XHCI_PORT_PLS_MASK) >> 5;

            if ((state & XHCI_PORT_PED) != 0u &&
                (speed_now != XHCI_SPEED_SUPER || pls == XHCI_PLS_U0)) {
                break;
            }

            if (now_us() - enabled > XHCI_RESET_ENABLE_US) {
                *final = state;
                return false;
            }

            state = op_read(address);
        }
    }

    /* The change bits after the enable: the connect and the reset of this
     * attempt are read and cleared, so the next scan does not see them. */
    op_write(address, state | XHCI_PORT_CHANGE);
    *final = op_read(address);

    if ((*final & XHCI_PORT_PED) == 0u) {
        return false;
    }

    *speed = (*final & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;

    return true;
}

/* Reset one port that has a device, and answer its speed and the PORTSC it
 * had before the reset. A port with nothing on it is not an error and says
 * nothing; a port with a device that does not come up is a warning with the
 * two PORTSC of the attempts, because that line is what a person sends when
 * the keyboard of a laptop does not work. */
static bool port_reset(k_u32_t port, k_u32_t *speed, k_u32_t *before)
{
    k_u32_t address = g_port_base + (port - 1u) * 16u;
    k_u32_t state = op_read(address);
    k_u32_t final = state;
    k_u32_t attempt;

    *before = state;

    if ((state & XHCI_PORT_CCS) == 0u) {
        return false;
    }

    if (!port_power(address, state)) {
        return false;
    }

    kdebug("usb: port %u: PORTSC %08x before the reset", (unsigned)port, (unsigned)state);

    for (attempt = 0; attempt < XHCI_RESET_ATTEMPTS; attempt++) {
        if (port_reset_once(address, &final, speed)) {
            kdebug("usb: port %u: PORTSC %08x after the reset (attempt %u, speed %s)",
                   (unsigned)port, (unsigned) final, (unsigned)(attempt + 1u), speed_name(*speed));

            /* The specification gives the device a moment after the reset of
             * its port before it answers the first command (Linux spends
             * 10 ms here), and a controller of a real laptop is not QEMU
             * about it. */
            delay_ms(XHCI_RESET_SETTLE_MS);

            return true;
        }

        /* A device that left during the reset is not a port of retries, and
         * a machine of a person unplugs one while it boots. */
        if ((op_read(address) & XHCI_PORT_CCS) == 0u) {
            kinfo("usb: the port %u lost its device during its reset (PORTSC %08x)", (unsigned)port,
                  (unsigned) final);
            return false;
        }

        /* The core of Linux tries the reset five times: a USB2 port whose
         * device was still settling comes up on the second one. */
        delay_ms(XHCI_RESET_SETTLE_MS);
    }

    kwarn("usb: the port %u is not enabled after its reset (PORTSC %08x -> %08x, %u attempt(s))",
          (unsigned)port, (unsigned)state, (unsigned) final, (unsigned)attempt);

    return false;
}

/*
 * The root ports of the controller, changed since the last look: the connect
 * change bit of PORTSC is the hot-plug of a laptop. A port that has a device
 * is reset and enumerated, a port that lost one has its devices taken away.
 * The change bits are written back as ones, which is how the register clears
 * them, before the reset reads the port again.
 */
static void root_port_scan(void)
{
    k_u32_t port;

    for (port = 1; port <= g_max_ports; port++) {
        k_u32_t address = g_port_base + (port - 1u) * 16u;
        k_u32_t state = op_read(address);
        k_u32_t change = state & XHCI_PORT_CHANGE;

        if (change == 0u) {
            continue;
        }

        op_write(address, state | change);

        if ((state & XHCI_PORT_CSC) == 0u) {
            continue;
        }

        if ((state & XHCI_PORT_CCS) == 0u) {
            if (detach_below(port, 0u, 0u) != 0u) {
                kinfo("xhci: the device on the port %u is gone", (unsigned)port);
            }
            continue;
        }

        {
            struct xhci_where where;
            k_u32_t speed = 0;
            k_u32_t before = 0;

            if (!port_reset(port, &speed, &before)) {
                continue;
            }

            kinfo("xhci: port %u: a device arrived at speed %u (%s), PORTSC %08x -> %08x",
                  (unsigned)port, (unsigned)speed, speed_name(speed), (unsigned)before,
                  (unsigned)op_read(address));

            where.root_port = (k_u8_t)port;
            where.depth = 0u;
            where.parent_slot = 0u;
            where.route = 0u;

            (void)enumerate_device(&where, speed);
        }
    }
}

/* --- the controller -------------------------------------------------------- */

/* The scratchpad buffers of the controller: the array of their addresses is
 * what the slot 0 of DCBAA names, and the pages are the memory of the heap,
 * which the identity map of the port makes an address a device can read. */
static bool scratchpad_alloc(k_u32_t pages)
{
    k_u32_t index;
    k_u32_t size = 4096u;

    if (pages == 0u) {
        return true;
    }

    if (pages > XHCI_SCRATCH_MAX) {
        kwarn("xhci: the controller asks %u scratchpad buffers, the driver has %u", (unsigned)pages,
              (unsigned)XHCI_SCRATCH_MAX);
        return false;
    }

    for (index = 0; index < pages; index++) {
        k_u8_t *raw = (k_u8_t *)k_port_alloc((k_size_t)size + 4096u, false);
        k_u64_t address;

        if (raw == NULL) {
            return false;
        }

        g_scratch_raw[index] = raw;
        address = ((k_u64_t)(uintptr_t)raw + 4095u) & ~(k_u64_t)4095u;
        memset((void *)(uintptr_t)address, 0, size);
        g_scratch_array[index] = port_x86_64_bus_phys((const void *)(uintptr_t)address);
    }

    g_dcbaa[0] = trb_phys(g_scratch_array);

    return true;
}

/* Where the controller is and how big its window is: the answer is the
 * physical address of the BAR and its size, or false when the function has
 * no memory window this driver can map. */
static bool controller_bar(const struct port_x86_64_pci_device *device, k_u64_t *base,
                           k_u32_t *size)
{
    k_u32_t low = port_x86_64_pci_read32(device, PCI_BAR0);
    k_u32_t high = 0;

    if ((low & PCI_BAR_IO) != 0u) {
        return false;
    }

    if (((low >> 1) & 0x3u) == 0x2u) {
        high = port_x86_64_pci_read32(device, (k_u8_t)(PCI_BAR0 + 4u));
    }

    *base = (k_u64_t)(low & 0xFFFFFFF0u) | ((k_u64_t)high << 32);
    *size = port_x86_64_pci_bar_size(device, PCI_BAR0);

    return *base != 0u && *size != 0u;
}

static bool controller_init(k_u64_t base, k_u32_t size)
{
    k_u64_t mapped;
    k_u64_t offset;
    k_u32_t pages_of_scratch;
    k_u32_t capability_length;
    k_u32_t version;
    k_u32_t parameters;
    k_u32_t parameters2;
    k_u32_t db_offset;
    k_u32_t rt_offset;

    __asm__ volatile("" : : : "memory");

    offset = base & 0x1FFFFFull;
    mapped = PORT_X86_64_XHCI_VIRT;

    if (port_x86_64_paging_map_mmio(mapped, base, (k_size_t)size) == 0u) {
        kwarn("xhci: the window of the controller at %x is not mappable", (unsigned)base);
        return false;
    }

    g_cap = (volatile k_u8_t *)(uintptr_t)(mapped + offset);

    capability_length = *(volatile k_u32_t *)(g_cap + 0u) & 0xFFu;
    version = (k_u32_t)(*(volatile k_u16_t *)(g_cap + XHCI_CAP_HCIVERSION));
    parameters = *(volatile k_u32_t *)(g_cap + XHCI_CAP_HCSPARAMS1);
    db_offset = *(volatile k_u32_t *)(g_cap + XHCI_CAP_DBOFF) & 0xFFFFFFFCu;
    rt_offset = *(volatile k_u32_t *)(g_cap + XHCI_CAP_RTSOFF) & 0xFFFFFFE0u;

    g_max_slots = parameters & 0xFFu;
    g_max_ports = (parameters >> 24) & 0xFFu;
    g_ppc = (parameters & 0x00000008u) != 0u;

    if (g_max_slots == 0u || g_max_slots > 64u) {
        kwarn("xhci: the controller reports %u slots", (unsigned)g_max_slots);
        g_max_slots = g_max_slots > 64u ? 64u : g_max_slots;
    }

    if (g_max_slots == 0u) {
        return false;
    }

    g_op = g_cap + capability_length;
    g_db = g_cap + db_offset;
    g_rt = g_cap + rt_offset;
    g_port_base = XHCI_OP_PORTS;

    parameters2 = *(volatile k_u32_t *)(g_cap + XHCI_CAP_HCSPARAMS2);
    pages_of_scratch = ((parameters2 >> 21) & 0x1Fu) | (((parameters2 >> 27) & 0x1Fu) << 5);

    /* A controller that the firmware left running is stopped and reset
     * before this driver programs a ring: the rings it had are the memory of
     * a driver this kernel does not know. */
    if ((op_read(XHCI_OP_USBCMD) & XHCI_CMD_RUN) != 0u) {
        op_write(XHCI_OP_USBCMD, op_read(XHCI_OP_USBCMD) & ~XHCI_CMD_RUN);

        if (!wait_until(XHCI_OP_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED, 100u, 10000000u)) {
            kwarn("xhci: the controller did not stop");
            return false;
        }
    }

    if (!wait_until(XHCI_OP_USBSTS, XHCI_STS_CNR, 0u, 1000u, 10000000u)) {
        kwarn("xhci: the controller is not ready");
        return false;
    }

    op_write(XHCI_OP_USBCMD, op_read(XHCI_OP_USBCMD) | XHCI_CMD_RESET);

    if (!wait_until(XHCI_OP_USBSTS, XHCI_STS_CNR, 0u, 1000u, 10000000u)) {
        kwarn("xhci: the reset of the controller did not finish");
        return false;
    }

    /* The rings, which the reset emptied. */
    memset(g_cmd_ring, 0, sizeof(g_cmd_ring));
    memset(g_event_ring, 0, sizeof(g_event_ring));
    memset(g_dcbaa, 0, sizeof(g_dcbaa));
    g_cmd_index = 0;
    g_cmd_cycle = 1;
    g_evt_index = 0;
    g_evt_cycle = 1;
    ring_link(g_cmd_ring, XHCI_CMD_TRBS, g_cmd_cycle);

    if (!scratchpad_alloc(pages_of_scratch)) {
        return false;
    }

    op_write(XHCI_OP_CONFIG, g_max_slots);
    op_write64(XHCI_OP_DCBAAP, trb_phys(g_dcbaa));
    op_write64(XHCI_OP_CRCR, trb_phys(g_cmd_ring) | 1u);

    /* The event ring: one segment of sixty-four TRBs, and the table of the
     * segments is a physical address of its own. */
    g_erst[0] = trb_phys(g_event_ring);
    g_erst[1] = XHCI_EVENT_TRBS;

    rt_write(XHCI_RT_ERSTSZ, 1u);
    rt_write64(XHCI_RT_ERSTBA, trb_phys(g_erst));
    rt_write64(XHCI_RT_ERDP, trb_phys(g_event_ring));

    /* The interrupts of the controller are enabled by the caller, after the
     * line of the PCI function is known and has a body in the table. A
     * controller that does not run is a controller without a driver. */
    op_write(XHCI_OP_USBCMD, XHCI_CMD_RUN);

    if (!wait_until(XHCI_OP_USBSTS, XHCI_STS_HALTED, 0u, 500u, 10000000u)) {
        kwarn("xhci: the controller did not start");
        return false;
    }

    if ((op_read(XHCI_OP_USBSTS) & XHCI_STS_ERROR) != 0u) {
        kwarn("xhci: the controller reports a host system error");
    }

    g_running = true;

    kinfo("xhci: controller at %x, version %u.%u, %u port(s), %u slot(s)",
          (unsigned)(base & 0xFFFFFFFFu), (unsigned)(version >> 8), (unsigned)(version & 0xFFu),
          (unsigned)g_max_ports, (unsigned)g_max_slots);

    return true;
}

/* --- the line of the controller -------------------------------------------- */

/*
 * The message-signalled interrupt of the function: a message written into the
 * configuration space and delivered by the local APIC of a core, with no
 * line of the 8259 to share and no redirection entry of the I/O APIC to
 * program for a PCI line whose polarity the firmware does not describe. The
 * capability is a record of the list at 0x34; the control word says whether
 * the address is 32 or 64 bits and the enable bit is its lowest.
 *
 * The answer is false when the function has no MSI: the legacy INTx line of
 * the PCI bus is the other way and it is taken below.
 */
static bool msi_enable(const struct port_x86_64_pci_device *device, k_u32_t vector)
{
    k_u16_t status = port_x86_64_pci_read16(device, PCI_STATUS);
    k_u32_t pointer;
    k_u32_t guard;

    if ((status & PCI_STATUS_CAP) == 0u) {
        return false;
    }

    pointer = port_x86_64_pci_read16(device, PCI_CAP_PTR) & 0xFCu;

    for (guard = 0; guard < PCI_MSI_CAP_MAX && pointer >= 0x40u; guard++) {
        k_u32_t header = port_x86_64_pci_read32(device, (k_u8_t)pointer);
        k_u8_t id = (k_u8_t)(header & 0xFFu);

        if (id == PCI_CAP_ID_MSI) {
            k_u16_t control = (k_u16_t)((header >> 16) & 0xFFFFu);
            bool wide = (control & PCI_MSI_64BIT) != 0u;
            k_u32_t destination = PCI_APIC_ADDRESS | (port_x86_64_apic_id() << 12);

            port_x86_64_pci_write32(device, (k_u8_t)(pointer + PCI_MSI_ADDR), destination);

            if (wide) {
                port_x86_64_pci_write32(device, (k_u8_t)(pointer + PCI_MSI_ADDR_HIGH), 0u);
            }

            port_x86_64_pci_write32(
                device, (k_u8_t)(pointer + (wide ? PCI_MSI_DATA + 4u : PCI_MSI_DATA)), vector);
            port_x86_64_pci_write16(device, (k_u8_t)(pointer + PCI_MSI_CTRL),
                                    (k_u16_t)(control | PCI_MSI_ENABLE));

            return true;
        }

        pointer = (header >> 8) & 0xFCu;
    }

    return false;
}

/*
 * The message table of MSI-X, which is the other form of the same idea: the
 * capability names a table in one of the windows of the function, the entry
 * of vector 0 is a message and a mask, and the enable bit of the control is
 * the switch. The window is the one the controller already has mapped, so
 * the write of an entry is a store into g_cap at the offset the capability
 * names (the base address register 0 is the window this driver reads).
 */
static bool msix_enable(const struct port_x86_64_pci_device *device, k_u32_t vector)
{
    k_u16_t status = port_x86_64_pci_read16(device, PCI_STATUS);
    k_u32_t pointer;
    k_u32_t guard;

    if ((status & PCI_STATUS_CAP) == 0u) {
        return false;
    }

    pointer = port_x86_64_pci_read16(device, PCI_CAP_PTR) & 0xFCu;

    for (guard = 0; guard < PCI_MSI_CAP_MAX && pointer >= 0x40u; guard++) {
        k_u32_t header = port_x86_64_pci_read32(device, (k_u8_t)pointer);
        k_u8_t id = (k_u8_t)(header & 0xFFu);

        if (id == PCI_CAP_ID_MSIX) {
            k_u16_t control = (k_u16_t)((header >> 16) & 0xFFFFu);
            k_u32_t table = port_x86_64_pci_read32(device, (k_u8_t)(pointer + PCI_MSIX_TABLE));
            volatile k_u32_t *entry;
            k_u32_t destination = PCI_APIC_ADDRESS | (port_x86_64_apic_id() << 12);

            /* The table has to be window 0 of the function: this driver maps
             * that window and not the others of the configuration space. */
            if (((table >> 29) & 0x7u) != 0u) {
                return false;
            }

            entry = (volatile k_u32_t *)(g_cap + (table & 0xFFFFFFF8u));

            entry[0] = destination;
            entry[1] = 0u;
            entry[2] = vector;
            entry[3] = 0u; /* the vector is open */

            __asm__ volatile("" : : : "memory");

            port_x86_64_pci_write16(device, (k_u8_t)(pointer + PCI_MSI_CTRL),
                                    (k_u16_t)(control | PCI_MSIX_ENABLE));

            return true;
        }

        pointer = (header >> 8) & 0xFCu;
    }

    return false;
}

/*
 * The line of a function that has no MSI: the INTx pin the firmware routed,
 * read out of the configuration space. The 8259 takes it when the port kept
 * the legacy chip; a machine whose two lines are on the I/O APIC does not
 * have a redirection entry for a PCI line that this port can describe (the
 * table of the firmware names the ISA lines only), and the controller keeps
 * the poll rather than an interrupt that arrives with the wrong polarity.
 */
static bool legacy_enable(const struct port_x86_64_pci_device *device)
{
    k_u16_t line_and_pin = port_x86_64_pci_read16(device, 0x3Cu);
    k_u8_t line = (k_u8_t)(line_and_pin & 0xFFu);
    k_u8_t pin = (k_u8_t)((line_and_pin >> 8) & 0xFFu);

    if (pin == 0u || pin == 0xFFu || line >= 16u) {
        return false;
    }

    if (port_x86_64_irq_on_ioapic()) {
        kinfo("xhci: the function names the line %u, but the machine routes its lines on the "
              "I/O APIC and no entry describes a PCI line; the event ring is polled",
              (unsigned)line);
        return false;
    }

    port_x86_64_idt_set(0x20u + line, isr_irq_xhci);
    port_x86_64_irq_unmask(line);

    return true;
}

/* The handler of the line: the pending bit of the interrupter is cleared and
 * the event handler is told (the end of the interrupt is the busy bit of the
 * event ring, written with the dequeue the pump has). The ring itself is not
 * read here: this driver sends one command at a time and waits for its
 * completion in the task that asked, and a second reader of the ring in the
 * handler would take an event from under that wait. The pump of the console
 * is the reader, and the interrupt is what makes an idle core answer a key
 * before its next nap. */
void port_x86_64_irq_xhci(k_u64_t *frame)
{
    K_UNUSED(frame);

    if (g_running) {
        /* The pending bit is written with a one to clear it (RW1C) and the
         * enable with a one to keep it: a zero there would shut the line. */
        rt_write(XHCI_RT_IMAN, XHCI_IMAN_IP | XHCI_IMAN_IE);

        /*
         * The busy bit of the event ring is written only when there is
         * nothing left in it. A completion the pump has not read yet is an
         * event the controller still calls "not consumed", and writing the
         * dequeue behind it asks QEMU (and a real controller) for another
         * interrupt at once - a storm of one event. The pump clears the bit
         * with the event it reads (event_pop writes ERDP).
         */
        if (event_peek() == NULL) {
            rt_write64(XHCI_RT_ERDP, trb_phys(&g_event_ring[g_evt_index]) | XHCI_ERDP_EHB);
        }

        g_irq_count++;

        /* The kernel of the boot has no task to wake and no console of a
         * person yet: the flag is read by the pump once the machine is up. */
        if (k_port_task_current() != NULL) {
            k_console_input_notify();
        }
    }

    port_x86_64_apic_eoi();
}

/* The vector of the line is in the table and the function raises it: the
 * interrupter is told to report, and the pending bit of the controller is
 * cleared with the one of an event that was never read. */
static void controller_irq_start(void)
{
    rt_write(XHCI_RT_IMAN, XHCI_IMAN_IP | XHCI_IMAN_IE);
    op_write(XHCI_OP_USBSTS, XHCI_STS_EVENT);
    op_write(XHCI_OP_USBCMD, op_read(XHCI_OP_USBCMD) | XHCI_CMD_INTE);
}

/*
 * The line of the controller, in the order of the price: MSI (one vector,
 * one message, no shared line), then the INTx pin of the function on a
 * machine that kept the 8259, then the poll the driver has always had. The
 * event ring is consumed by the pump either way.
 */
static void controller_line(const struct port_x86_64_pci_device *device)
{
    g_irq_vector = XHCI_VECTOR;

    if (msi_enable(device, g_irq_vector)) {
        port_x86_64_idt_set(g_irq_vector, isr_irq_xhci);
        controller_irq_start();
        g_irq = true;
        kinfo("xhci: the line of the controller is MSI, vector %u", (unsigned)g_irq_vector);
        return;
    }

    if (msix_enable(device, g_irq_vector)) {
        port_x86_64_idt_set(g_irq_vector, isr_irq_xhci);
        controller_irq_start();
        g_irq = true;
        kinfo("xhci: the line of the controller is MSI-X, vector %u", (unsigned)g_irq_vector);
        return;
    }

    if (legacy_enable(device)) {
        controller_irq_start();
        g_irq = true;
        kinfo("xhci: the line of the controller is INTx, vector %u", (unsigned)g_irq_vector);
        return;
    }

    kinfo("xhci: the controller has no line this port can open, the event ring is polled");
}

/* --- the reports of the devices -------------------------------------------- */

static void usb_event_push(k_u16_t code, k_u16_t value)
{
    k_u32_t next = (g_in_event_head + 1u) % XHCI_INPUT_RING;

    if (next == g_in_event_tail) {
        return;
    }

    g_in_events[g_in_event_head].ms = (k_u32_t)k_port_uptime_ms();
    g_in_events[g_in_event_head].code = code;
    g_in_events[g_in_event_head].value = value;
    g_in_event_head = next;
}

static void usb_char_push(k_u8_t character)
{
    k_u32_t next = (g_in_char_head + 1u) % XHCI_INPUT_RING;

    if (next == g_in_char_tail) {
        return;
    }

    g_in_chars[g_in_char_head] = character;
    g_in_char_head = next;

    /* A character that arrives before the scheduler exists (the boot of a
     * person who types at a hub's port while it is enumerated) has no task to
     * wake: the ring keeps it for the console. */
    if (k_port_task_current() != NULL) {
        k_console_input_notify();
    }
}

/* One report of the keyboard: the events of the interface and the characters
 * of the console, as the PS/2 pushes them from its handler. */
static void keyboard_report(k_u32_t buffer)
{
    struct k_input_hid_event events[16];
    k_size_t count;
    k_size_t index;

    count = k_input_hid_decode(&g_hid, g_kbd_report[buffer], events,
                               sizeof(events) / sizeof(events[0]));

    for (index = 0; index < count; index++) {
        if (events[index].key != K_INPUT_KEY_NONE && !events[index].repeat) {
            usb_event_push(events[index].key, events[index].value);
        }

        if (events[index].character != 0 && !g_capture) {
            usb_char_push((k_u8_t)events[index].character);
        }
    }
}

/* One report of the mouse: the two differences, the button and the wheel, as
 * the ring of `/dev/input0` names them. The mouse types no character. */
static void mouse_report(k_u32_t buffer, k_u32_t length)
{
    struct k_input_hid_event events[8];
    k_size_t count;
    k_size_t index;

    if (length > K_INPUT_HID_MOUSE_REPORT) {
        length = K_INPUT_HID_MOUSE_REPORT;
    }

    count = k_input_hid_mouse_decode(&g_mouse_hid, g_mouse_report[buffer], (k_size_t)length, events,
                                     sizeof(events) / sizeof(events[0]));

    for (index = 0; index < count; index++) {
        if (events[index].key != K_INPUT_KEY_NONE) {
            usb_event_push(events[index].key, events[index].value);
        }
    }
}

/* Put the next report of the endpoint in the ring: the producer follows the
 * consumer around the ring, and the link at the end of it turns the cycle
 * bit over. */
static void keyboard_arm(void)
{
    k_u32_t index = g_keyboard.index;
    struct xhci_trb *trb;

    if (index == XHCI_EP_TRBS - 1u) {
        ring_link(g_kbd_ring, XHCI_EP_TRBS, g_keyboard.cycle);
        g_keyboard.cycle ^= 1u;
        g_keyboard.index = 0;
        index = 0;
    }

    trb = &g_kbd_ring[index];
    trb->param = trb_phys(g_kbd_report[index]);
    trb->status = K_INPUT_HID_REPORT;
    trb->control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | g_keyboard.cycle;
    g_keyboard.index = index + 1u;
}

static void mouse_arm(void)
{
    k_u32_t index = g_mouse.index;
    struct xhci_trb *trb;

    if (index == XHCI_EP_TRBS - 1u) {
        ring_link(g_mouse_ring, XHCI_EP_TRBS, g_mouse.cycle);
        g_mouse.cycle ^= 1u;
        g_mouse.index = 0;
        index = 0;
    }

    trb = &g_mouse_ring[index];
    trb->param = trb_phys(g_mouse_report[index]);
    trb->status = g_mouse.max_packet;
    trb->control = XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC | g_mouse.cycle;
    g_mouse.index = index + 1u;
}

/* The report of a ring of the reports: the buffer is the one the completion
 * names, and the event answers how many bytes the device sent (a mouse
 * without a wheel sends three of the four). */
static void report_event(k_u32_t slot, k_u32_t dci, k_u64_t param, k_u32_t status, k_u32_t code)
{
    if (g_keyboard.present && g_keyboard.slot == slot && g_keyboard.dci == dci) {
        k_u64_t ring = trb_phys(g_kbd_ring);
        k_u32_t buffer;

        if (param < ring || param >= ring + (k_u64_t)XHCI_KBD_BUFFERS * 16u ||
            (param - ring) % 16u != 0u) {
            keyboard_arm();
            return;
        }

        buffer = (k_u32_t)((param - ring) / 16u);

        if (code == XHCI_CODE_SUCCESS || code == XHCI_CODE_SHORT) {
            keyboard_report(buffer);
        } else if (code != XHCI_CODE_STALL) {
            kwarn("usb: a report of the keyboard ended with the code %u", (unsigned)code);
        }

        memset(g_kbd_report[buffer], 0, K_INPUT_HID_REPORT);
        keyboard_arm();

        __asm__ volatile("" : : : "memory");
        doorbell(g_keyboard.slot, g_keyboard.dci);

        return;
    }

    if (g_mouse.present && g_mouse.slot == slot && g_mouse.dci == dci) {
        k_u64_t ring = trb_phys(g_mouse_ring);
        k_u32_t buffer;
        k_u32_t left = XHCI_TRB_LENGTH(status);

        if (param < ring || param >= ring + (k_u64_t)XHCI_KBD_BUFFERS * 16u ||
            (param - ring) % 16u != 0u) {
            mouse_arm();
            return;
        }

        buffer = (k_u32_t)((param - ring) / 16u);

        if (code == XHCI_CODE_SUCCESS || code == XHCI_CODE_SHORT) {
            k_u32_t length = left <= g_mouse.max_packet ? g_mouse.max_packet - left : 0u;

            mouse_report(buffer, length);
        } else if (code != XHCI_CODE_STALL) {
            kwarn("usb: a report of the mouse ended with the code %u", (unsigned)code);
        }

        memset(g_mouse_report[buffer], 0, K_INPUT_HID_MOUSE_REPORT);
        mouse_arm();

        __asm__ volatile("" : : : "memory");
        doorbell(g_mouse.slot, g_mouse.dci);
    }
}

/*
 * Read the event ring and do what the events say. A transfer event of the
 * keyboard or of the mouse names the TRB that completed, and the report is
 * the buffer of that TRB; the TRB goes back to the ring with the next report
 * before the caller returns, so the device is never left without a place to
 * put a key or a movement.
 *
 * The call is the pump of the driver and it runs in the task that asks for a
 * key: the critical section keeps two readers of the console of two cores
 * from taking one report twice.
 */
static void usb_service(void)
{
    struct xhci_trb *event;

    if (!g_running) {
        return;
    }

    k_port_enter_critical();

    while ((event = event_peek()) != NULL) {
        k_u32_t type = XHCI_TRB_GET_TYPE(event->control);
        k_u32_t slot = XHCI_TRB_SLOT(event->control);
        k_u32_t dci = XHCI_TRB_ENDPOINT(event->control);

        if (type == XHCI_TRB_TRANSFER) {
            if ((g_keyboard.present && g_keyboard.slot == slot && g_keyboard.dci == dci) ||
                (g_mouse.present && g_mouse.slot == slot && g_mouse.dci == dci)) {
                k_u64_t param = event->param;
                k_u32_t status = event->status;

                event_pop();
                report_event(slot, dci, param, status, XHCI_TRB_CODE(status));
                continue;
            }

            /* A transfer of a class: the completion belongs to a call that
             * waits for it, and the pump leaves it in the queue instead of
             * reading it as one of its own. */
            if ((g_xfer_head + 1u) % XHCI_XFER_QUEUE == g_xfer_tail) {
                break;
            }

            {
                struct xhci_trb copy = *event;

                event_pop();
                queue_put(g_xfer_queue, &g_xfer_head, &g_xfer_tail, XHCI_XFER_QUEUE, &copy);
            }

            continue;
        }

        if (type == XHCI_TRB_COMMAND_COMPLETION) {
            if ((g_cmd_head + 1u) % XHCI_CMD_QUEUE == g_cmd_tail) {
                break;
            }

            {
                struct xhci_trb copy = *event;

                event_pop();
                queue_put(g_cmd_queue, &g_cmd_head, &g_cmd_tail, XHCI_CMD_QUEUE, &copy);
            }

            continue;
        }

        /* A port status change is the news the scan of the ports reads out
         * of the registers; the event itself is consumed and dropped. */
        event_pop();
    }

    k_port_exit_critical();
}

/* --- the hot-plug of the ports --------------------------------------------- */

static bool g_scanning;

/*
 * The scan of the ports after the boot: the root ports of the controller at
 * every pump (a read of PORTSC each) and the ports below a hub four times a
 * second. The scan issues commands of the controller, so it runs in the task
 * and not inside the critical section of the pump; a second core that finds
 * the flag set leaves the scan to the one that has it.
 */
static void usb_scan(void)
{
    k_u32_t now;

    if (!g_running) {
        return;
    }

    k_port_enter_critical();

    if (g_scanning) {
        k_port_exit_critical();
        return;
    }

    g_scanning = true;
    k_port_exit_critical();

    if (k_mutex_lock(&g_xhci_lock) == 0) {
        k_u32_t index;

        root_port_scan();

        now = (k_u32_t)k_port_uptime_ms();

        if ((k_i32_t)(now - g_next_hub_scan_ms) >= 0) {
            g_next_hub_scan_ms = now + XHCI_HUB_SCAN_MS;

            for (index = 0; index < XHCI_HUBS; index++) {
                if (g_hubs[index].used) {
                    hub_scan_ports(&g_hubs[index]);
                }
            }
        }

        k_mutex_unlock(&g_xhci_lock);
    }

    k_port_enter_critical();
    g_scanning = false;
    k_port_exit_critical();

    /* The first interrupt of the controller is a fact of the boot and the
     * test reads it: the handler cannot log, because it may have interrupted
     * the log itself. */
    if (g_irq && !g_irq_first_seen && g_irq_count != 0u) {
        g_irq_first_seen = true;
        kinfo("xhci: the event ring raised the line %u (%u interrupt(s))", (unsigned)g_irq_vector,
              (unsigned)g_irq_count);
    }
}

/* --- the calls of the port ------------------------------------------------ */

bool port_x86_64_usb_keyboard(void)
{
    return g_keyboard.present;
}

bool port_x86_64_usb_pointer(void)
{
    return g_mouse.present;
}

int port_x86_64_usb_poll(void)
{
    k_u32_t tail;
    int character = -1;

    usb_scan();

    if (!g_keyboard.present) {
        return -1;
    }

    k_port_enter_critical();
    usb_service();

    tail = g_in_char_tail;
    if (tail != g_in_char_head) {
        character = (int)g_in_chars[tail];
        g_in_char_tail = (tail + 1u) % XHCI_INPUT_RING;
    }
    k_port_exit_critical();

    return character;
}

bool port_x86_64_usb_event(struct input_event *out)
{
    k_u32_t tail;

    if (out == NULL) {
        return false;
    }

    usb_scan();

    k_port_enter_critical();
    usb_service();

    tail = g_in_event_tail;
    if (tail == g_in_event_head) {
        k_port_exit_critical();
        return false;
    }

    out->ms = g_in_events[tail].ms;
    out->code = g_in_events[tail].code;
    out->value = g_in_events[tail].value;
    g_in_event_tail = (tail + 1u) % XHCI_INPUT_RING;
    k_port_exit_critical();

    return true;
}

void port_x86_64_usb_capture(bool on)
{
    k_port_enter_critical();
    g_capture = on;

    /* The rings are emptied when the owner changes, as the rings of the
     * PS/2 are: a key typed at the prompt is not a key of an interface. */
    g_in_char_tail = g_in_char_head;
    g_in_event_tail = g_in_event_head;
    k_port_exit_critical();
}

void port_x86_64_xhci_init(void)
{
    struct port_x86_64_pci_device device;
    k_u64_t base = 0;
    k_u32_t size = 0;
    k_u16_t vendor;
    k_u16_t product;
    k_u32_t port;
    k_u32_t speed = 0;

    g_keyboard.present = false;
    g_mouse.present = false;
    g_running = false;
    g_scanning = false;
    g_irq = false;
    g_irq_first_seen = false;
    memset(g_class_device, 0, sizeof(g_class_device));
    memset(g_hubs, 0, sizeof(g_hubs));
    memset(g_slot_pool, 0, sizeof(g_slot_pool));

    if (!port_x86_64_pci_find_class(0x0Cu, 0x03u, 0x30u, &device)) {
        kdebug("xhci: the machine has no USB 3.0 controller");
        return;
    }

    vendor = port_x86_64_pci_read16(&device, PCI_VENDOR);
    product = port_x86_64_pci_read16(&device, PCI_DEVICE);

    if (!controller_bar(&device, &base, &size)) {
        kwarn("xhci: %04x:%04x has no memory window", (unsigned)vendor, (unsigned)product);
        return;
    }

    /* The bus master and the memory window: a controller that cannot read
     * the rings of the driver is a controller of nothing. */
    port_x86_64_pci_enable(&device, PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER);

    if (!controller_init(base, size)) {
        return;
    }

    /* The line is opened before the ports: the enumeration of a hub takes
     * long enough that a device behind it can raise its connect change in
     * the middle of the boot. */
    controller_line(&device);

    {
        k_u32_t attached = 0u;
        k_u32_t enumerated = 0u;

        for (port = 1; port <= g_max_ports; port++) {
            struct xhci_where where;
            k_u32_t before = 0;

            if ((op_read(g_port_base + (port - 1u) * 16u) & XHCI_PORT_CCS) != 0u) {
                attached++;
            }

            if (!port_reset(port, &speed, &before)) {
                continue;
            }

            /* The line a person reads when a port of a laptop misbehaves:
             * the register before the reset and after it, and the speed the
             * controller named. */
            kinfo("xhci: port %u: a device at speed %u (%s), PORTSC %08x -> %08x", (unsigned)port,
                  (unsigned)speed, speed_name(speed), (unsigned)before,
                  (unsigned)op_read(g_port_base + (port - 1u) * 16u));

            where.root_port = (k_u8_t)port;
            where.depth = 0u;
            where.parent_slot = 0u;
            where.route = 0u;

            /* Every port is enumerated: a laptop has a keyboard and a stick
             * at the same time, and the second one is not a reason to stop. */
            if (enumerate_device(&where, speed)) {
                enumerated++;
            }
        }

        /* A controller with devices on its ports and none of them
         * enumerated is not "a machine without a keyboard": it is a driver
         * that did not finish one device, and the line says which number to
         * look at. */
        if (attached > 0u && enumerated == 0u) {
            kwarn("usb: %u port(s) of the controller have a device and none of them enumerated",
                  (unsigned)attached);
        }
    }

    g_next_hub_scan_ms = (k_u32_t)k_port_uptime_ms() + XHCI_HUB_SCAN_MS;

    if (!g_keyboard.present) {
        kinfo("usb: the controller answered, no keyboard of the boot protocol on it");
    }

    if (!g_mouse.present) {
        kdebug("usb: no mouse of the boot protocol on the controller");
    }
}
