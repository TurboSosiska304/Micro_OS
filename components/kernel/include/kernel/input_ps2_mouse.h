/*
 * Micro OS — a PS/2 mouse packet as the pointer events of the machine.
 *
 * The second port of the 8042 speaks a stream of bytes: three of them are
 * one packet (the buttons, the sign and the overflow of the two axes, then
 * the two differences), and a mouse with a wheel sends a fourth, the signed
 * notches of the wheel. The sequence 0xF3 200 / 0xF3 100 / 0xF3 80 and the
 * answer to 0xF2 turn a plain mouse into that IntelliMouse, and the driver of
 * the port tells this decoder which packet it reads.
 *
 * This is the portable half of the driver: it knows bytes and events and no
 * controller, so a host test drives it with the packets of the specification
 * and the port of the machine is the caller. The events it answers are the
 * same shape as the ones of the USB mouse (kernel/input_hid.h), because the
 * two devices are one pointer in /dev/input0.
 */
#ifndef MICRO_OS_KERNEL_INPUT_PS2_MOUSE_H
#define MICRO_OS_KERNEL_INPUT_PS2_MOUSE_H

#include "kernel/input_hid.h"
#include "kernel/types.h"

#define K_INPUT_PS2_MOUSE_PACKET 3u
#define K_INPUT_PS2_MOUSE_PACKET_WHEEL 4u

struct k_input_ps2_mouse_state {
    k_u8_t packet[K_INPUT_PS2_MOUSE_PACKET_WHEEL];
    k_u32_t count;
    k_u32_t packet_size; /* 3 or 4 */
    k_u8_t buttons;      /* the mask of the buttons of the last packet */
    k_u32_t overflow_x;
    k_u32_t overflow_y;
};

/* The state of a mouse that has sent nothing yet. A wheel mouse sends four
 * bytes in a packet; a plain one sends three. */
void k_input_ps2_mouse_init(struct k_input_ps2_mouse_state *state, bool wheel);

/*
 * Feed one byte of the stream. When a whole packet is ready the events of it
 * are written into out, capped at max, and the answer is the number of
 * events written; otherwise the answer is 0. A packet whose first byte has
 * bit 3 clear, or whose overflow bit is set on an axis, is dropped: the
 * stream resynchronises on the next byte that can be a first byte, and the
 * overflow is counted instead of becoming a jump of the pointer.
 */
k_size_t k_input_ps2_mouse_feed(struct k_input_ps2_mouse_state *state, k_u8_t byte,
                                struct k_input_hid_event *out, k_size_t max);

#endif /* MICRO_OS_KERNEL_INPUT_PS2_MOUSE_H */
