/*
 * Micro OS — the PS/2 mouse packet as events (kernel/input_ps2_mouse.h).
 *
 * The decoder is pure: it takes bytes and gives the same pointer events the
 * USB mouse gives, so the port only owns the 8042 and the ring. The packets
 * here are the ones of the specification: three bytes for a plain mouse and
 * four for an IntelliMouse with a wheel.
 */
#include "ktest.h"
#include "tests/tests.h"

#include "kernel/input.h"
#include "kernel/input_ps2_mouse.h"

/* A plain motion: dx on the x axis and dy turned over on the y axis, because
 * the device counts up and the screen counts down. */
static void test_motion(void)
{
    struct k_input_ps2_mouse_state state;
    struct k_input_hid_event events[8];
    k_size_t count;

    ktest_begin("ps2 mouse: a plain packet is a motion on the two axes");

    k_input_ps2_mouse_init(&state, false);
    KT_CHECK_INT_EQ((int)k_input_ps2_mouse_feed(&state, 0x08u, events, 8u), 0);
    KT_CHECK_INT_EQ((int)k_input_ps2_mouse_feed(&state, 0x03u, events, 8u), 0);
    count = k_input_ps2_mouse_feed(&state, 0x02u, events, 8u);

    KT_CHECK_INT_EQ((int)count, 2);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_POINTER_X);
    KT_CHECK_INT_EQ((k_i16_t)events[0].value, 3);
    KT_CHECK_INT_EQ(events[1].key, K_INPUT_KEY_POINTER_Y);
    KT_CHECK_INT_EQ((k_i16_t)events[1].value, -2);
}

/* A negative difference is a signed 9-bit number: the sign bits of the first
 * byte extend the two bytes of the motion. */
static void test_negative(void)
{
    struct k_input_ps2_mouse_state state;
    struct k_input_hid_event events[8];
    k_size_t count;

    ktest_begin("ps2 mouse: a negative motion is a signed difference");

    k_input_ps2_mouse_init(&state, false);
    (void)k_input_ps2_mouse_feed(&state, 0x38u, events, 8u);   /* bit3, x-, y- */
    (void)k_input_ps2_mouse_feed(&state, 0xFFu, events, 8u);   /* dx = -1 */
    count = k_input_ps2_mouse_feed(&state, 0xF0u, events, 8u); /* dy = -16 */

    KT_CHECK_INT_EQ((int)count, 2);
    KT_CHECK_INT_EQ((k_i16_t)events[0].value, -1);
    KT_CHECK_INT_EQ((k_i16_t)events[1].value, 16);
}

/* The button is an edge: the press, no event while it is held, the release. */
static void test_button(void)
{
    struct k_input_ps2_mouse_state state;
    struct k_input_hid_event events[8];
    k_size_t count;

    ktest_begin("ps2 mouse: the left button is an edge of the packet");

    k_input_ps2_mouse_init(&state, false);

    (void)k_input_ps2_mouse_feed(&state, 0x09u, events, 8u);
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    count = k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    KT_CHECK_INT_EQ((int)count, 1);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_BUTTON_LEFT);
    KT_CHECK_INT_EQ(events[0].value, INPUT_VALUE_PRESS);

    (void)k_input_ps2_mouse_feed(&state, 0x09u, events, 8u);
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    count = k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    KT_CHECK_INT_EQ((int)count, 0);

    (void)k_input_ps2_mouse_feed(&state, 0x08u, events, 8u);
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    count = k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    KT_CHECK_INT_EQ((int)count, 1);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_BUTTON_LEFT);
    KT_CHECK_INT_EQ(events[0].value, INPUT_VALUE_RELEASE);
}

/* An overflow bit means the device could not express the motion. The packet
 * is dropped and counted, so the pointer does not jump by a made-up number. */
static void test_overflow(void)
{
    struct k_input_ps2_mouse_state state;
    struct k_input_hid_event events[8];
    k_size_t count;

    ktest_begin("ps2 mouse: an overflow drops the packet and counts it");

    k_input_ps2_mouse_init(&state, false);

    (void)k_input_ps2_mouse_feed(&state, 0x48u, events, 8u); /* x overflow */
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    count = k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    KT_CHECK_INT_EQ((int)count, 0);
    KT_CHECK_INT_EQ((int)state.overflow_x, 1);
    KT_CHECK_INT_EQ((int)state.overflow_y, 0);

    (void)k_input_ps2_mouse_feed(&state, 0x88u, events, 8u); /* y overflow */
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    count = k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    KT_CHECK_INT_EQ((int)count, 0);
    KT_CHECK_INT_EQ((int)state.overflow_x, 1);
    KT_CHECK_INT_EQ((int)state.overflow_y, 1);
}

/* A byte without bit 3 at the start of a packet is not a packet: the decoder
 * drops it and the next proper packet still reads cleanly. */
static void test_resync(void)
{
    struct k_input_ps2_mouse_state state;
    struct k_input_hid_event events[8];
    k_size_t count;

    ktest_begin("ps2 mouse: a byte without bit 3 resynchronises the stream");

    k_input_ps2_mouse_init(&state, false);
    KT_CHECK_INT_EQ((int)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u), 0);

    (void)k_input_ps2_mouse_feed(&state, 0x08u, events, 8u);
    (void)k_input_ps2_mouse_feed(&state, 0x01u, events, 8u);
    count = k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    KT_CHECK_INT_EQ((int)count, 1);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_POINTER_X);
    KT_CHECK_INT_EQ((k_i16_t)events[0].value, 1);
}

/* The fourth byte of an IntelliMouse is the wheel, signed. */
static void test_wheel(void)
{
    struct k_input_ps2_mouse_state state;
    struct k_input_hid_event events[8];
    k_size_t count;

    ktest_begin("ps2 mouse: the fourth byte of an IntelliMouse is the wheel");

    k_input_ps2_mouse_init(&state, true);

    (void)k_input_ps2_mouse_feed(&state, 0x08u, events, 8u);
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    count = k_input_ps2_mouse_feed(&state, 0x01u, events, 8u);
    KT_CHECK_INT_EQ((int)count, 1);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_POINTER_WHEEL);
    KT_CHECK_INT_EQ((k_i16_t)events[0].value, 1);

    (void)k_input_ps2_mouse_feed(&state, 0x08u, events, 8u);
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    (void)k_input_ps2_mouse_feed(&state, 0x00u, events, 8u);
    count = k_input_ps2_mouse_feed(&state, 0xFFu, events, 8u);
    KT_CHECK_INT_EQ((int)count, 1);
    KT_CHECK_INT_EQ((k_i16_t)events[0].value, -1);
}

void test_ps2_mouse(void)
{
    test_motion();
    test_negative();
    test_button();
    test_overflow();
    test_resync();
    test_wheel();
}
