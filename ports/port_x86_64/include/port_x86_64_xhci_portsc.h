/*
 * Micro OS — the portable part of an xHCI root port (xHCI 1.2b §5.4.8).
 *
 * PORTSC is not an ordinary read/modify/write register. PED and the change
 * bits are write-one-to-clear, PR is write-one-to-set, and writing PED and PR
 * together is undefined. Keep the field decoding and the safe write values
 * here so the driver and the host tests use the same rules without MMIO.
 */
#ifndef MICRO_OS_PORT_X86_64_XHCI_PORTSC_H
#define MICRO_OS_PORT_X86_64_XHCI_PORTSC_H

#include <stdbool.h>
#include <stdint.h>

/* Operational Base + (400h + (10h * (n - 1))), n = 1..MaxPorts. */
#define XHCI_OP_PORTS 0x400u
#define XHCI_PORT_STRIDE 16u

#define XHCI_PORT_CCS 0x00000001u
#define XHCI_PORT_PED 0x00000002u
#define XHCI_PORT_TM 0x00000004u
#define XHCI_PORT_OCA 0x00000008u
#define XHCI_PORT_PR 0x00000010u
#define XHCI_PORT_PLS_SHIFT 5u
#define XHCI_PORT_PLS_MASK 0x000001E0u
#define XHCI_PORT_PP 0x00000200u
#define XHCI_PORT_SPEED_SHIFT 10u
#define XHCI_PORT_SPEED_MASK 0x00003C00u
#define XHCI_PORT_PIC_MASK 0x0000C000u
#define XHCI_PORT_LWS 0x00010000u
#define XHCI_PORT_CSC 0x00020000u
#define XHCI_PORT_PEC 0x00040000u
#define XHCI_PORT_WRC 0x00080000u
#define XHCI_PORT_OCC 0x00100000u
#define XHCI_PORT_PRC 0x00200000u
#define XHCI_PORT_PLC 0x00400000u
#define XHCI_PORT_CEC 0x00800000u
#define XHCI_PORT_CHANGE 0x00FE0000u
#define XHCI_PORT_WAKE_MASK 0x0E000000u
#define XHCI_PORT_DR 0x40000000u
#define XHCI_PORT_WPR 0x80000000u

/* Read-only and sticky read/write fields may be preserved in a write. PED,
 * PR, LWS, WPR and every change bit may not: a one in PED disables the port,
 * and a one in a change bit clears the event it names. */
#define XHCI_PORT_NEUTRAL_MASK                                                                     \
    (XHCI_PORT_CCS | XHCI_PORT_TM | XHCI_PORT_OCA | XHCI_PORT_PLS_MASK | XHCI_PORT_PP |            \
     XHCI_PORT_SPEED_MASK | XHCI_PORT_PIC_MASK | XHCI_PORT_WAKE_MASK | XHCI_PORT_DR)

#define XHCI_SPEED_FULL 1u
#define XHCI_SPEED_LOW 2u
#define XHCI_SPEED_HIGH 3u
#define XHCI_SPEED_SUPER 4u

#define XHCI_PLS_U0 0u
#define XHCI_PLS_U1 1u
#define XHCI_PLS_U2 2u
#define XHCI_PLS_U3 3u
#define XHCI_PLS_DISABLED 4u
#define XHCI_PLS_RXDETECT 5u
#define XHCI_PLS_INACTIVE 6u
#define XHCI_PLS_POLLING 7u
#define XHCI_PLS_RECOVERY 8u
#define XHCI_PLS_HOT_RESET 9u
#define XHCI_PLS_COMPLIANCE 10u

static inline uint32_t xhci_portsc_pls(uint32_t state)
{
    return (state & XHCI_PORT_PLS_MASK) >> XHCI_PORT_PLS_SHIFT;
}

static inline uint32_t xhci_portsc_speed(uint32_t state)
{
    return (state & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT;
}

/* A write that preserves the port state without setting or clearing a status
 * bit. Linux calls the same operation xhci_port_state_to_neutral(). */
static inline uint32_t xhci_portsc_neutral(uint32_t state)
{
    return state & XHCI_PORT_NEUTRAL_MASK;
}

/* Clear only change bits that were observed. Writing zero to an RW1CS bit has
 * no effect, while writing one to a reserved USB2 bit is not a legal clear. */
static inline uint32_t xhci_portsc_clear_changes(uint32_t state)
{
    return xhci_portsc_neutral(state) | (state & XHCI_PORT_CHANGE);
}

static inline uint32_t xhci_portsc_start_reset(uint32_t state)
{
    return xhci_portsc_neutral(state) | XHCI_PORT_PR;
}

/* A completed reset enters Enabled/U0 and reports a protocol speed. A USB3
 * port still in Polling has not finished link training, even if PED is set. */
static inline bool xhci_portsc_enabled(uint32_t state, bool usb3)
{
    uint32_t speed;

    if ((state & (XHCI_PORT_CCS | XHCI_PORT_PED)) != (XHCI_PORT_CCS | XHCI_PORT_PED) ||
        xhci_portsc_pls(state) != XHCI_PLS_U0) {
        return false;
    }

    speed = xhci_portsc_speed(state);
    return usb3 ? speed >= XHCI_SPEED_SUPER : speed >= XHCI_SPEED_FULL && speed <= XHCI_SPEED_HIGH;
}

/* §4.19.1.1: a USB2 port in Polling is the Disabled state and needs PR.
 * §4.19.1.2.5: a USB3 port cannot be reset from Disconnected (RxDetect),
 * Powered-off or Disabled, and Polling is left to finish link training. */
static inline bool xhci_portsc_can_reset(uint32_t state, bool usb3)
{
    uint32_t pls;

    if ((state & (XHCI_PORT_CCS | XHCI_PORT_PR | XHCI_PORT_OCA)) != XHCI_PORT_CCS) {
        return false;
    }

    pls = xhci_portsc_pls(state);

    if (usb3) {
        return pls == XHCI_PLS_U0 || pls == XHCI_PLS_U1 || pls == XHCI_PLS_U2 ||
               pls == XHCI_PLS_U3 || pls == XHCI_PLS_INACTIVE || pls == XHCI_PLS_RECOVERY;
    }

    return pls == XHCI_PLS_POLLING ||
           ((state & XHCI_PORT_PED) != 0u &&
            (pls == XHCI_PLS_U0 || pls == XHCI_PLS_U2 || pls == XHCI_PLS_U3));
}

/* USB2 TRSTRCY is 10 ms; the USB3 reset recovery budget is 50 ms. */
static inline uint32_t xhci_portsc_settle_ms(uint32_t state, bool usb3)
{
    return usb3 || xhci_portsc_speed(state) >= XHCI_SPEED_SUPER ? 50u : 10u;
}

static inline uint32_t xhci_portsc_offset(uint32_t port)
{
    return port == 0u ? 0u : XHCI_OP_PORTS + (port - 1u) * XHCI_PORT_STRIDE;
}

/* Fields of a Supported Protocol Capability header (xHCI 1.2b §7.2). */
static inline uint32_t xhci_protocol_major(uint32_t header)
{
    return (header >> 24) & 0xFFu;
}

static inline uint32_t xhci_protocol_minor(uint32_t header)
{
    return (header >> 16) & 0xFFu;
}

static inline uint32_t xhci_protocol_first_port(uint32_t compatible)
{
    return compatible & 0xFFu;
}

static inline uint32_t xhci_protocol_port_count(uint32_t compatible)
{
    return (compatible >> 8) & 0xFFu;
}

#endif /* MICRO_OS_PORT_X86_64_XHCI_PORTSC_H */
