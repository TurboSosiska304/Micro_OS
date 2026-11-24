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
 * keyboard a person has under the hands.
 *
 * The interrupts of the controller are off (USBCMD.INTE) and the event ring
 * is read by the console and by the reader of /dev/input0 when they are
 * asked for a key: a boot of this release has one keyboard and no need of a
 * line of its own, and a driver that polls is a driver that cannot lose the
 * machine to a storm of interrupts from a device it does not understand.
 */
#include "port_x86_64_xhci.h"

#include <string.h>

#include "kernel/console.h"
#include "kernel/input.h"
#include "kernel/input_hid.h"
#include "kernel/log.h"
#include "kernel/port/lock.h"
#include "kernel/port/mem.h"
#include "kernel/port/time.h"
#include "port_x86_64.h"
#include "port_x86_64_console.h"
#include "port_x86_64_input.h"
#include "port_x86_64_pci.h"

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

#define XHCI_STS_HALTED 0x00000001u
#define XHCI_STS_ERROR 0x00000004u
#define XHCI_STS_EVENT 0x00000008u
#define XHCI_STS_CNR 0x00000800u

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
#define XHCI_DEVICES 4u
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

/* The scratchpad buffers of a controller that asks for them: the array of
 * their addresses, which the controller reads, and the pages themselves,
 * which this driver allocates on the heap and keeps for the life of the
 * machine. */
#define XHCI_SCRATCH_MAX 8u
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

static k_u32_t g_ep0_index[XHCI_DEVICES];
static k_u32_t g_ep0_cycle[XHCI_DEVICES];

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
} g_keyboard;

static struct k_input_hid_state g_hid;

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
        struct xhci_trb *event = event_peek();

        if (event != NULL) {
            k_u32_t type = XHCI_TRB_GET_TYPE(event->control);
            k_u64_t param = event->param;
            k_u32_t status = event->status;
            k_u32_t control = event->control;

            event_pop();

            if (type == XHCI_TRB_COMMAND_COMPLETION && param == address) {
                *code = (k_u8_t)XHCI_TRB_CODE(status);
                *slot_out = XHCI_TRB_SLOT(control);
                return true;
            }

            /* A port status change of a device that arrived at the same
             * moment is not the answer and is not lost: the driver reset
             * its ports already, and the pump reads the rest. */
            continue;
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

/* The slot context of an input context: the speed, the port and how many
 * endpoint contexts follow it (1 for the endpoint 0, the highest index for a
 * device with an endpoint 1 IN). */
static void slot_context(k_u8_t *input, k_u32_t speed, k_u32_t port, k_u32_t entries)
{
    context_put32(input, 32u, (entries << 27) | (speed << 20));
    context_put32(input, 36u, port << 16);
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

/* The address of the device: the slot is given its context and the ring of
 * its endpoint 0, and the controller sends SET_ADDRESS to it. */
static bool address_device(k_u32_t slot, k_u32_t port, k_u32_t speed, k_u32_t packet)
{
    struct xhci_trb trb;
    k_u8_t code = 0;
    k_u32_t answer = 0;
    k_u32_t index = slot - 1u;

    memset(g_input, 0, sizeof(g_input));
    context_put32(g_input, 0u, 0u);                    /* the contexts to drop */
    context_put32(g_input, 4u, (1u << 0) | (1u << 1)); /* the slot and the endpoint 0 */
    slot_context(g_input, speed, port, 1u);
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
static bool evaluate_ep0(k_u32_t slot, k_u32_t port, k_u32_t speed, k_u32_t packet)
{
    struct xhci_trb trb;
    k_u8_t code = 0;
    k_u32_t answer = 0;
    k_u32_t index = slot - 1u;

    /* The slot and the endpoint 0 are the two contexts of this command: the
     * add flags say which of them the controller reads. */
    memset(g_input, 0, sizeof(g_input));
    context_put32(g_input, 0u, 0u);
    context_put32(g_input, 4u, (1u << 0) | (1u << 1));
    slot_context(g_input, speed, port, 1u);
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

/* The endpoint of the reports: the context of the endpoint 1 IN and its
 * ring, and the device is configured with it. */
static bool configure_keyboard(k_u32_t slot, k_u32_t port, k_u32_t speed, k_u32_t dci,
                               k_u32_t interval, k_u32_t max_packet)
{
    struct xhci_trb trb;
    k_u8_t code = 0;
    k_u32_t answer = 0;

    g_keyboard.index = 0;
    g_keyboard.cycle = 1;
    memset(g_kbd_ring, 0, sizeof(g_kbd_ring));
    ring_link(g_kbd_ring, XHCI_EP_TRBS, g_keyboard.cycle);

    memset(g_input, 0, sizeof(g_input));
    context_put32(g_input, 0u, 0u);
    context_put32(g_input, 4u, (1u << 0) | (1u << dci));
    slot_context(g_input, speed, port, dci);
    endpoint_context(g_input, dci, XHCI_EP_TYPE_INTERRUPT_IN, interval, max_packet, g_kbd_ring);

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

    if (g_ep0_index[index] == XHCI_EP_TRBS - 1u) {
        ring_link(g_ep0_ring[index], XHCI_EP_TRBS, g_ep0_cycle[index]);
        g_ep0_cycle[index] ^= 1u;
        g_ep0_index[index] = 0;
    }

    slot = &g_ep0_ring[index][g_ep0_index[index]];
    *slot = *trb;
    slot->control |= g_ep0_cycle[index];
    g_ep0_index[index]++;

    return slot;
}

/* The answer of the endpoint 0: a transfer event of this slot and this
 * endpoint, whose completion code says how the transfer ended. */
static bool ep0_wait(k_u32_t slot, k_u32_t dci, k_u8_t *code, k_u32_t *left)
{
    k_u64_t deadline = k_port_uptime_ms() + 500u;

    for (;;) {
        struct xhci_trb *event = event_peek();

        if (event != NULL) {
            k_u32_t type = XHCI_TRB_GET_TYPE(event->control);
            k_u32_t status = event->status;
            k_u32_t control = event->control;

            event_pop();

            if (type == XHCI_TRB_TRANSFER && XHCI_TRB_SLOT(control) == slot &&
                XHCI_TRB_ENDPOINT(control) == dci) {
                *code = (k_u8_t)XHCI_TRB_CODE(status);
                *left = XHCI_TRB_LENGTH(status);
                return true;
            }

            continue;
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
    k_u32_t ring_index = slot - 1u;
    bool in = (request_type & 0x80u) != 0u;
    k_u8_t code = 0;
    k_u32_t left = 0;

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

    if (!ep0_wait(slot, 1u, &code, &left)) {
        return false;
    }

    return code == XHCI_CODE_SUCCESS || code == XHCI_CODE_SHORT;
}

/* --- the configuration descriptor ----------------------------------------- */

struct usb_keyboard {
    k_u8_t interface;
    k_u8_t endpoint;
    k_u16_t max_packet;
    k_u8_t interval;
    bool found;
};

/* The interface of a keyboard of the boot protocol and its endpoint of the
 * reports: class 3 (HID), sub-class 1 and protocol 1, and an interrupt
 * endpoint of the device to the host. */
static void parse_configuration(const k_u8_t *descriptor, k_u32_t length,
                                struct usb_keyboard *keyboard)
{
    k_u32_t offset = 0;
    bool in_keyboard = false;

    keyboard->found = false;

    while (offset + 2u <= length) {
        k_u8_t size = descriptor[offset];
        k_u8_t type = descriptor[offset + 1u];

        if (size < 2u || offset + size > length) {
            return;
        }

        if (type == 4u && size >= 9u) {
            in_keyboard = descriptor[offset + 5u] == 3u && descriptor[offset + 6u] == 1u &&
                          descriptor[offset + 7u] == 1u;

            if (in_keyboard) {
                keyboard->interface = descriptor[offset + 2u];
            }
        } else if (type == 5u && size >= 7u && in_keyboard) {
            k_u8_t address = descriptor[offset + 2u];
            k_u8_t attributes = descriptor[offset + 3u];

            if ((address & 0x80u) != 0u && (attributes & 0x03u) == 0x03u) {
                keyboard->endpoint = address;
                keyboard->max_packet =
                    (k_u16_t)(descriptor[offset + 4u] | ((k_u16_t)descriptor[offset + 5u] << 8));
                keyboard->interval = descriptor[offset + 6u];
                keyboard->found = true;
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
}

/*
 * The enumeration of the device of one port: a slot, an address, the
 * descriptors of it, and the endpoint of the reports when the device is a
 * keyboard. The answer is true when a keyboard of the boot protocol was
 * found and is reporting.
 */
static bool enumerate_device(k_u32_t port, k_u32_t speed)
{
    struct usb_keyboard keyboard;
    k_u8_t code = 0;
    k_u32_t slot = 0;
    k_u32_t index;
    k_u32_t packet = ep0_packet(speed);
    k_u32_t total;
    k_u16_t vendor = 0;
    k_u16_t product = 0;

    if (!command_simple(XHCI_TRB_ENABLE_SLOT, 0, &code, &slot) || code != XHCI_CODE_SUCCESS ||
        slot == 0u) {
        kwarn("xhci: the controller gave no slot for the port %u", (unsigned)port);
        return false;
    }

    if (slot > XHCI_DEVICES) {
        /* The pool of the contexts of this driver is small on purpose: four
         * devices of a machine are a keyboard, a mouse and two ports of a
         * hub that this release does not enumerate. */
        kinfo("xhci: the slot %u is past the pool of the driver", (unsigned)slot);
        disable_slot(slot);
        return false;
    }

    index = slot - 1u;
    memset(g_device_ctx[index], 0, 2048u);
    g_dcbaa[slot] = trb_phys(g_device_ctx[index]);
    memset(g_ep0_ring[index], 0, sizeof(g_ep0_ring[index]));
    g_ep0_index[index] = 0;
    g_ep0_cycle[index] = 1;
    ring_link(g_ep0_ring[index], XHCI_EP_TRBS, g_ep0_cycle[index]);

    if (!address_device(slot, port, speed, packet)) {
        disable_slot(slot);
        return false;
    }

    /* The first eight bytes of the descriptor of the device: the largest
     * packet of its endpoint 0 is the seventh, and a full-speed device that
     * answers with sixteen or thirty-two needs a context of that size before
     * the rest of the descriptor can arrive without a babble. */
    if (!control_transfer(slot, 0x80u, 6u, 0x0100u, 0u, 8u, g_desc)) {
        kinfo("usb: port %u: the device did not answer its descriptor", (unsigned)port);
        disable_slot(slot);
        return false;
    }

    {
        k_u32_t device_packet = g_desc[7];

        if ((device_packet == 8u || device_packet == 16u || device_packet == 32u ||
             device_packet == 64u) &&
            device_packet != packet) {
            if (!evaluate_ep0(slot, port, speed, device_packet)) {
                kinfo("usb: port %u: the endpoint 0 did not take its size", (unsigned)port);
                disable_slot(slot);
                return false;
            }

            packet = device_packet;
        }
    }

    if (!control_transfer(slot, 0x80u, 6u, 0x0100u, 0u, 18u, g_desc)) {
        kinfo("usb: port %u: the device has no full descriptor", (unsigned)port);
        disable_slot(slot);
        return false;
    }

    vendor = (k_u16_t)(g_desc[8] | ((k_u16_t)g_desc[9] << 8));
    product = (k_u16_t)(g_desc[10] | ((k_u16_t)g_desc[11] << 8));

    if (!control_transfer(slot, 0x80u, 6u, 0x0200u, 0u, 9u, g_desc)) {
        kinfo("usb: port %u: the device has no configuration", (unsigned)port);
        disable_slot(slot);
        return false;
    }

    total = (k_u32_t)g_desc[2] | ((k_u32_t)g_desc[3] << 8);
    if (total < 9u || total > sizeof(g_desc)) {
        total = 9u;
    }

    if (total > 9u && !control_transfer(slot, 0x80u, 6u, 0x0200u, 0u, (k_u16_t)total, g_desc)) {
        disable_slot(slot);
        return false;
    }

    parse_configuration(g_desc, total, &keyboard);

    if (!keyboard.found) {
        kinfo("usb: port %u: %04x:%04x is not a keyboard of the boot protocol", (unsigned)port,
              (unsigned)vendor, (unsigned)product);
        disable_slot(slot);
        return false;
    }

    /* SET_CONFIGURATION: the configuration of the descriptor, with no
     * payload and the value at byte five. */
    if (!control_transfer(slot, 0x00u, 9u, g_desc[5], 0u, 0u, NULL)) {
        kinfo("usb: port %u: the device refused its configuration", (unsigned)port);
        disable_slot(slot);
        return false;
    }

    /* SET_PROTOCOL (boot): a keyboard of the HID has a report of eight
     * bytes only in the boot protocol, and this driver reads the eight. */
    if (!control_transfer(slot, 0x21u, 0x0Bu, 0u, keyboard.interface, 0u, NULL)) {
        kwarn("usb: the keyboard did not take the boot protocol, its report is the one of the "
              "HID");
    }

    {
        k_u32_t dci = endpoint_dci(keyboard.endpoint);
        k_u32_t interval = endpoint_interval(keyboard.interval, speed);
        k_u32_t max_packet = keyboard.max_packet & 0x07FFu;

        if (!configure_keyboard(slot, port, speed, dci, interval, max_packet)) {
            kinfo("usb: port %u: the endpoint of the reports did not come up", (unsigned)port);
            disable_slot(slot);
            return false;
        }

        g_keyboard.present = true;
        g_keyboard.slot = (k_u8_t)slot;
        g_keyboard.dci = (k_u8_t)dci;
        g_keyboard.interface = keyboard.interface;
        g_keyboard.max_packet = (k_u16_t)max_packet;
        k_input_hid_init(&g_hid);

        /* Seven reports are in the ring of the endpoint before the first key
         * arrives: a person who types while the boot still prints a line
         * does not lose the first letter to the speed of the poll. */
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

        kinfo("usb: keyboard %04x:%04x on port %u, slot %u, interface %u, %u bytes",
              (unsigned)vendor, (unsigned)product, (unsigned)port, (unsigned)slot,
              (unsigned)keyboard.interface, (unsigned)max_packet);
    }

    return true;
}

/* --- the port of the controller -------------------------------------------- */

/* Reset one port that has a device, and answer its speed. A port with nothing
 * on it is not an error and says nothing. */
static bool port_reset(k_u32_t port, k_u32_t *speed)
{
    k_u32_t address = g_port_base + (port - 1u) * 16u;
    k_u32_t state = op_read(address);
    k_u64_t deadline;
    k_u32_t guard;

    if ((state & XHCI_PORT_CCS) == 0u) {
        return false;
    }

    /* The power of the port first: a port that is not powered is a port
     * whose reset is not a reset. */
    if ((state & XHCI_PORT_PP) == 0u) {
        op_write(address, state | XHCI_PORT_PP);

        for (guard = 0; guard < 100u; guard++) {
            if ((op_read(address) & XHCI_PORT_PP) != 0u) {
                break;
            }
        }
    }

    /* The reset bit, with the change bits of the moment as they are: the
     * write-back of a one to a change bit clears it and never sets it. */
    state = op_read(address);
    op_write(address,
             (state & (XHCI_PORT_CCS | XHCI_PORT_PP | XHCI_PORT_PLS_MASK | XHCI_PORT_SPEED_MASK)) |
                 XHCI_PORT_PR);

    /* A reset of a port takes tens of milliseconds; the clock is the limit
     * and the count is the guard of a machine whose clock is not there. */
    deadline = k_port_uptime_ms() + 500u;

    for (guard = 0; guard < 1000000u; guard++) {
        state = op_read(address);

        if ((state & XHCI_PORT_PR) == 0u || k_port_uptime_ms() > deadline) {
            break;
        }
    }

    state = op_read(address);

    /* The change bits of the reset and of the connection are cleared by the
     * write of a one; PED says the port is enabled and speaks. */
    op_write(address, state | XHCI_PORT_CHANGE);
    state = op_read(address);

    if ((state & XHCI_PORT_PED) == 0u) {
        kwarn("usb: the port %u is not enabled after its reset", (unsigned)port);
        return false;
    }

    *speed = (state & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;

    /* The specification gives the device a moment after the reset of its
     * port before it answers the first command (Linux spends 10 ms here),
     * and a controller of a real laptop is not QEMU about it. */
    delay_ms(10u);

    return true;
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

    /* The interrupts of the controller stay off: the console polls the
     * event ring. A controller that does not run is a controller without a
     * driver. */
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

/* --- the reports of the keyboard ------------------------------------------- */

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
    k_console_input_notify();
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

/*
 * Read the event ring and do what the events say. A transfer event of the
 * keyboard names the TRB that completed, and the report is the buffer of
 * that TRB; the TRB goes back to the ring with the next report before the
 * caller returns, so the device is never left without a place to put a key.
 *
 * The call is the pump of the driver and it runs in the task that asks for a
 * key: the critical section keeps two readers of the console of two cores
 * from taking one report twice.
 */
static void usb_service(void)
{
    struct xhci_trb *event;
    k_u64_t ring = trb_phys(g_kbd_ring);

    if (!g_running || !g_keyboard.present) {
        return;
    }

    while ((event = event_peek()) != NULL) {
        k_u32_t type = XHCI_TRB_GET_TYPE(event->control);
        k_u64_t param = event->param;
        k_u32_t status = event->status;
        k_u32_t control = event->control;
        k_u32_t buffer;

        event_pop();

        if (type != XHCI_TRB_TRANSFER) {
            continue;
        }

        if (XHCI_TRB_SLOT(control) != g_keyboard.slot ||
            XHCI_TRB_ENDPOINT(control) != g_keyboard.dci) {
            continue;
        }

        if (param < ring || param >= ring + (k_u64_t)XHCI_KBD_BUFFERS * 16u ||
            (param - ring) % 16u != 0u) {
            keyboard_arm();
            continue;
        }

        buffer = (k_u32_t)((param - ring) / 16u);
        {
            k_u32_t code = XHCI_TRB_CODE(status);

            if (code == XHCI_CODE_SUCCESS || code == XHCI_CODE_SHORT) {
                keyboard_report(buffer);
            } else if (code != XHCI_CODE_STALL) {
                kwarn("usb: a report of the keyboard ended with the code %u", (unsigned)code);
            }
        }

        /* The report buffer is spent: the next one of the ring is armed with
         * an empty buffer. */
        memset(g_kbd_report[buffer], 0, K_INPUT_HID_REPORT);
        keyboard_arm();

        __asm__ volatile("" : : : "memory");
        doorbell(g_keyboard.slot, g_keyboard.dci);
    }
}

/* --- the calls of the port ------------------------------------------------ */

bool port_x86_64_usb_keyboard(void)
{
    return g_keyboard.present;
}

int port_x86_64_usb_poll(void)
{
    k_u32_t tail;
    int character = -1;

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

    if (out == NULL || !g_keyboard.present) {
        return false;
    }

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
    g_running = false;

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

    for (port = 1; port <= g_max_ports; port++) {
        if (!port_reset(port, &speed)) {
            continue;
        }

        kinfo("xhci: port %u: a device at speed %u", (unsigned)port, (unsigned)speed);

        if (enumerate_device(port, speed)) {
            /* One keyboard is the keyboard of the machine: a second one on
             * another port is a device of the same class and there is no
             * focus to give it. */
            break;
        }
    }

    if (!g_keyboard.present) {
        kinfo("usb: the controller answered, no keyboard of the boot protocol on it");
    }
}
