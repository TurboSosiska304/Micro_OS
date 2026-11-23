/*
 * Micro OS — the keys of the x86-64 port as events (ADR 0028).
 *
 * The keyboard of the machine speaks the PS/2 set 1 and the console reads it
 * as characters (src/ps2.c). A program with a focus needs the other half:
 * `Tab` and `Enter` are commands, `Ctrl` and `Shift` are modifiers with a
 * press and a release, and a key that is held is a state and not a stream of
 * bytes. The handler of IRQ1 therefore pushes one event beside every
 * character, and `/dev/input0` (src/inputdev.c) hands those events to the
 * program that opened the device.
 *
 * The event ring is the property of the port and not of the device: a machine
 * without the device still counts its keys, and the capture is what keeps the
 * characters of an interface out of the shell that waits for it.
 */
#ifndef MICRO_OS_PORT_X86_64_INPUT_H
#define MICRO_OS_PORT_X86_64_INPUT_H

#include "kernel/input.h"
#include "kernel/types.h"

/* Put `/dev/input0` in the devfs: called once at the boot, after the devfs
 * is mounted. */
void port_x86_64_input_register(void);

/* The next event of the keyboard, or false when the ring is empty. */
bool port_x86_64_input_event(struct input_event *out);

/*
 * The pointer of the machine (1.7.0). `port_x86_64_input_pointer` says whether
 * the second port of the controller answered at the boot, and the two counters
 * are the motions the mouse could not express in a packet (the overflow bits
 * of the first byte): an axis that overflowed is dropped and counted, and the
 * desktop reads the count when a person says the pointer jumped.
 */
bool port_x86_64_input_pointer(void);
k_u32_t port_x86_64_input_overflow(bool vertical);

/*
 * The byte of the second port: the body of IRQ12 (src/ps2.c). It is the same
 * body as the keyboard's, because the buffer of the controller is one and the
 * status byte says whose byte it is.
 */
void port_x86_64_irq_mouse(k_u64_t *frame);

/*
 * The keyboard belongs to one reader for as long as it is open: while the
 * capture is on, the keys are events and not characters. Taking it drops the
 * characters that are already waiting, and so does giving it back, so a key
 * of an interface does not arrive as a line of the shell after it ends.
 */
void port_x86_64_input_capture(bool on);

#endif /* MICRO_OS_PORT_X86_64_INPUT_H */
