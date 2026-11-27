/*
 * Micro OS — the PS/2 mouse packet (kernel/input_ps2_mouse.h).
 *
 * Three bytes are a packet of a plain mouse and four of an IntelliMouse: the
 * first carries the buttons and the sign and overflow of the two axes, the
 * next two the differences, and the fourth, when it is there, the notches of
 * the wheel. The parser is pure: it takes one byte at a time and gives the
 * events of a whole packet, so the port and the test drive the same code.
 */
#include "kernel/input_ps2_mouse.h"

#include <stddef.h>

void k_input_ps2_mouse_init(struct k_input_ps2_mouse_state *state, bool wheel)
{
    if (state == NULL) {
        return;
    }

    state->packet[0] = 0u;
    state->packet[1] = 0u;
    state->packet[2] = 0u;
    state->packet[3] = 0u;
    state->count = 0u;
    state->packet_size = wheel ? K_INPUT_PS2_MOUSE_PACKET_WHEEL : K_INPUT_PS2_MOUSE_PACKET;
    state->buttons = 0u;
    state->overflow_x = 0u;
    state->overflow_y = 0u;
}

static k_size_t ps2_emit(struct k_input_hid_event *out, k_size_t max, k_size_t count, k_u16_t key,
                         k_u16_t value)
{
    if (count < max) {
        out[count].key = key;
        out[count].value = value;
        out[count].character = 0;
        out[count].repeat = false;
    }

    return count + 1u;
}

k_size_t k_input_ps2_mouse_feed(struct k_input_ps2_mouse_state *state, k_u8_t byte,
                                struct k_input_hid_event *out, k_size_t max)
{
    k_size_t count = 0u;
    k_u8_t flags;
    k_i16_t dx;
    k_i16_t dy;
    k_u8_t buttons;

    if (state == NULL || out == NULL || max == 0u) {
        return 0u;
    }

    if (state->count == 0u) {
        /* Bit 3 is always set in the first byte of a packet. A byte
         * without it is the device resynchronising, not a start. */
        if ((byte & 0x08u) == 0u) {
            return 0u;
        }
    }

    state->packet[state->count] = byte;
    state->count++;

    if (state->count < state->packet_size) {
        return 0u;
    }

    state->count = 0u;
    flags = state->packet[0];

    /* An overflow means the device could not express the motion: the
     * packet is dropped and counted, because a made-up jump is worse
     * than a pointer that waits for the next motion. */
    if ((flags & 0xC0u) != 0u) {
        if ((flags & 0x40u) != 0u) {
            state->overflow_x++;
        }
        if ((flags & 0x80u) != 0u) {
            state->overflow_y++;
        }
        return 0u;
    }

    dx = (k_i16_t)state->packet[1];
    if ((flags & 0x10u) != 0u) {
        dx = (k_i16_t)(dx - 256);
    }

    dy = (k_i16_t)state->packet[2];
    if ((flags & 0x20u) != 0u) {
        dy = (k_i16_t)(dy - 256);
    }

    if (dx != 0) {
        count = ps2_emit(out, max, count, K_INPUT_KEY_POINTER_X, (k_u16_t)dx);
    }

    /* The device counts up and the screen counts down. */
    if (dy != 0) {
        count = ps2_emit(out, max, count, K_INPUT_KEY_POINTER_Y, (k_u16_t)(k_i16_t)-dy);
    }

    if (state->packet_size >= K_INPUT_PS2_MOUSE_PACKET_WHEEL && (k_i8_t)state->packet[3] != 0) {
        count = ps2_emit(out, max, count, K_INPUT_KEY_POINTER_WHEEL,
                         (k_u16_t)(k_i16_t)(k_i8_t)state->packet[3]);
    }

    buttons = (k_u8_t)(flags & 0x01u);
    if (buttons != (state->buttons & 0x01u)) {
        count = ps2_emit(out, max, count, K_INPUT_KEY_BUTTON_LEFT,
                         (buttons != 0u) ? INPUT_VALUE_PRESS : INPUT_VALUE_RELEASE);
    }

    state->buttons = flags;

    return count < max ? count : max;
}
