/*
 * Micro OS — the infrared pin of the StickC Plus2 (G19).
 *
 * The board carries an IR emitter and a red LED on one pin: the pin drives
 * the emitter's transistor in one direction and the LED in the other, so the
 * two can never shine at the same time. The rule of the driver is the rule of
 * the wiring, made explicit:
 *
 *   - the emitter owns the pin while a frame goes out. The LED module is
 *     suspended for that time, its light is dark, and a `led` command that
 *     arrives then answers "the IR emitter has G19" instead of fighting for
 *     the pad;
 *   - the LED owns the pin the rest of the time. An IR command makes the
 *     light dark before the first pulse (the pad is re-routed to the RMT
 *     peripheral) and leaves it dark afterwards: the emitter never glows by
 *     accident because a fade was in the middle of its way down.
 *
 * The receiver is not on the board. The StickC Plus2 has the emitter and no
 * IR receiver, so reception is offered for the demodulating element a person
 * wires to the free pin G26: `ir rx` waits for a frame there and answers "no
 * receiver" when the line stays idle. Nothing on the board proves reception
 * without that element, and the documentation says so.
 */
#ifndef MICRO_OS_DRIVERS_IR_H
#define MICRO_OS_DRIVERS_IR_H

#include "drivers/ir_nec.h"
#include "kernel/types.h"

/*
 * Send one frame of a NEC command from the emitter on G19: the carrier of
 * 38 kHz is made by the RMT peripheral, the frame by ir_nec_build().
 * Returns 0, -1 when the frame cannot be built, -2 when the RMT channel
 * cannot be made, -3 when the frame does not leave the peripheral.
 */
int drv_ir_send_nec(k_u8_t address, k_u8_t command);

/*
 * Wait up to `timeout_ms` for one NEC frame at the receiver on G26. Returns 0
 * and fills *frame for a frame the complement check accepts, 1 when the line
 * stayed idle (there is probably no receiver), -1 for a missing argument, -2
 * when the RMT channel cannot be made and -3 when something was received and
 * it was not a NEC frame.
 */
int drv_ir_receive_nec(struct ir_nec_frame *frame, k_u32_t timeout_ms);

/* Whether the emitter owns G19 now: the LED asks before it lights. */
bool drv_ir_owns_pin(void);

/* Register the module of the hardware doctor and the command of the shell. */
int drv_ir_register(void);

#endif /* MICRO_OS_DRIVERS_IR_H */
