/*
 * Micro OS — the fields and safe writes of an xHCI root port.
 *
 * PORTSC mixes read-only status, sticky controls, write-one-to-set and
 * write-one-to-clear fields. These tests replay the register without MMIO,
 * including the two values from the K25 laptop report, so a whole-register
 * write-back cannot quietly turn PED into a port disable again.
 */
#include "ktest.h"
#include "tests/tests.h"

#include "port_x86_64_xhci_portsc.h"

static uint32_t enabled_state(uint32_t speed, uint32_t pls)
{
    return XHCI_PORT_CCS | XHCI_PORT_PED | XHCI_PORT_PP | (speed << XHCI_PORT_SPEED_SHIFT) |
           (pls << XHCI_PORT_PLS_SHIFT);
}

void test_xhci_portsc(void)
{
    uint32_t value;
    uint32_t index;
    uint32_t state;

    /* K25: 000286e1 -> 000806e1. Both have CCS and a speed field, but neither
     * has PED and both are in Polling, so neither is an enabled port. */
    state = 0x000286e1u;
    KT_CHECK_INT_EQ(xhci_portsc_pls(state), XHCI_PLS_POLLING);
    KT_CHECK_INT_EQ(xhci_portsc_speed(state), XHCI_SPEED_FULL);
    KT_CHECK((state & XHCI_PORT_CCS) != 0u);
    KT_CHECK((state & XHCI_PORT_PED) == 0u);
    KT_CHECK((state & XHCI_PORT_PR) == 0u);
    KT_CHECK((state & XHCI_PORT_PP) != 0u);
    KT_CHECK((state & XHCI_PORT_PIC_MASK) == 0x00008000u);
    KT_CHECK_INT_EQ(xhci_portsc_neutral(state), 0x000086e1u);
    KT_CHECK_INT_EQ(xhci_portsc_clear_changes(state), 0x000286e1u);
    KT_CHECK_INT_EQ(xhci_portsc_start_reset(state), 0x000086f1u);
    KT_CHECK(!xhci_portsc_enabled(state, false));
    KT_CHECK(!xhci_portsc_enabled(state, true));

    state = 0x000806e1u;
    KT_CHECK_INT_EQ(xhci_portsc_pls(state), XHCI_PLS_POLLING);
    KT_CHECK_INT_EQ(xhci_portsc_speed(state), XHCI_SPEED_FULL);
    KT_CHECK((state & XHCI_PORT_CCS) != 0u);
    KT_CHECK((state & XHCI_PORT_PED) == 0u);
    KT_CHECK((state & XHCI_PORT_WRC) != 0u);
    KT_CHECK_INT_EQ(xhci_portsc_neutral(state), 0x000006e1u);
    KT_CHECK_INT_EQ(xhci_portsc_clear_changes(state), 0x000806e1u);
    KT_CHECK_INT_EQ(xhci_portsc_start_reset(state), 0x000006f1u);
    KT_CHECK(!xhci_portsc_enabled(state, false));
    KT_CHECK(!xhci_portsc_enabled(state, true));

    /* Every four-bit field is decoded without borrowing a neighbour. */
    for (index = 0; index < 16u; index++) {
        state = XHCI_PORT_CCS | (index << XHCI_PORT_PLS_SHIFT);
        KT_CHECK_INT_EQ(xhci_portsc_pls(state), index);
        state = XHCI_PORT_CCS | (index << XHCI_PORT_SPEED_SHIFT);
        KT_CHECK_INT_EQ(xhci_portsc_speed(state), index);
    }

    /* The masks keep status/control fields and drop every one-shot field. */
    state = 0xFFFFFFFFu;
    value = xhci_portsc_neutral(state);
    KT_CHECK_INT_EQ(value, XHCI_PORT_NEUTRAL_MASK);
    KT_CHECK((value & (XHCI_PORT_PED | XHCI_PORT_PR | XHCI_PORT_LWS | XHCI_PORT_WPR)) == 0u);
    KT_CHECK((value & XHCI_PORT_CHANGE) == 0u);
    KT_CHECK_INT_EQ(xhci_portsc_clear_changes(state), XHCI_PORT_NEUTRAL_MASK | XHCI_PORT_CHANGE);
    KT_CHECK_INT_EQ(xhci_portsc_start_reset(state), XHCI_PORT_NEUTRAL_MASK | XHCI_PORT_PR);

    /* A completed reset is CCS + PED + U0 + a speed valid for the protocol. */
    KT_CHECK(xhci_portsc_enabled(enabled_state(XHCI_SPEED_FULL, XHCI_PLS_U0), false));
    KT_CHECK(xhci_portsc_enabled(enabled_state(XHCI_SPEED_LOW, XHCI_PLS_U0), false));
    KT_CHECK(xhci_portsc_enabled(enabled_state(XHCI_SPEED_HIGH, XHCI_PLS_U0), false));
    KT_CHECK(xhci_portsc_enabled(enabled_state(XHCI_SPEED_SUPER, XHCI_PLS_U0), true));
    KT_CHECK(!xhci_portsc_enabled(enabled_state(XHCI_SPEED_SUPER, XHCI_PLS_POLLING), true));
    KT_CHECK(!xhci_portsc_enabled(enabled_state(XHCI_SPEED_FULL, XHCI_PLS_U0), true));
    KT_CHECK(!xhci_portsc_enabled(enabled_state(0u, XHCI_PLS_U0), false));
    KT_CHECK(!xhci_portsc_enabled(enabled_state(XHCI_SPEED_FULL, XHCI_PLS_U3), false));

    /* USB2 Polling is Disabled and may be reset. USB3 Polling is training and
     * RxDetect/Disabled are not legal source states for PR. */
    KT_CHECK(xhci_portsc_can_reset(0x000286e1u, false));
    KT_CHECK(!xhci_portsc_can_reset(0x000286e1u, true));
    KT_CHECK(
        !xhci_portsc_can_reset(XHCI_PORT_CCS | (XHCI_PLS_RXDETECT << XHCI_PORT_PLS_SHIFT), false));
    KT_CHECK(
        !xhci_portsc_can_reset(XHCI_PORT_CCS | (XHCI_PLS_DISABLED << XHCI_PORT_PLS_SHIFT), true));
    KT_CHECK(xhci_portsc_can_reset(enabled_state(XHCI_SPEED_SUPER, XHCI_PLS_U0), true));
    KT_CHECK(xhci_portsc_can_reset(enabled_state(XHCI_SPEED_FULL, XHCI_PLS_U3), false));
    KT_CHECK(
        !xhci_portsc_can_reset(enabled_state(XHCI_SPEED_FULL, XHCI_PLS_U0) | XHCI_PORT_PR, false));
    KT_CHECK(
        !xhci_portsc_can_reset(enabled_state(XHCI_SPEED_FULL, XHCI_PLS_U0) | XHCI_PORT_OCA, false));
    KT_CHECK(!xhci_portsc_can_reset(XHCI_PORT_PP, false));

    KT_CHECK_INT_EQ(xhci_portsc_settle_ms(enabled_state(XHCI_SPEED_FULL, XHCI_PLS_U0), false), 10u);
    KT_CHECK_INT_EQ(xhci_portsc_settle_ms(enabled_state(XHCI_SPEED_HIGH, XHCI_PLS_U0), false), 10u);
    KT_CHECK_INT_EQ(xhci_portsc_settle_ms(enabled_state(XHCI_SPEED_SUPER, XHCI_PLS_U0), true), 50u);
    KT_CHECK_INT_EQ(xhci_portsc_settle_ms(0u, true), 50u);

    /* PORTSC n is one-based: port 14 is 4D0h and port 15 is 4E0h. */
    KT_CHECK_INT_EQ(xhci_portsc_offset(0u), 0u);
    KT_CHECK_INT_EQ(xhci_portsc_offset(1u), 0x400u);
    KT_CHECK_INT_EQ(xhci_portsc_offset(2u), 0x410u);
    KT_CHECK_INT_EQ(xhci_portsc_offset(14u), 0x4D0u);
    KT_CHECK_INT_EQ(xhci_portsc_offset(15u), 0x4E0u);
    KT_CHECK_INT_EQ(xhci_portsc_offset(255u), 0x13E0u);

    /* Supported Protocol Capability: USB 3.10, ports 14..15. */
    state = (3u << 24) | (0x10u << 16) | (5u << 8) | 2u;
    value = (2u << 8) | 14u;
    KT_CHECK_INT_EQ(xhci_protocol_major(state), 3u);
    KT_CHECK_INT_EQ(xhci_protocol_minor(state), 0x10u);
    KT_CHECK_INT_EQ(xhci_protocol_first_port(value), 14u);
    KT_CHECK_INT_EQ(xhci_protocol_port_count(value), 2u);
}
