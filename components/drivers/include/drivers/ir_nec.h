/*
 * Micro OS — the frame of an infrared remote, as arithmetic.
 *
 * The emitter of the board has no protocol in it: an IR diode shines when the
 * pin is high, and a remote shines it in bursts of 38 kHz. The frame is a
 * list of pulses - the carrier on for a number of microseconds, then off for
 * another - and everything that decides what those numbers are is here, in a
 * file with no hardware in it, so the host tests can build a frame and read
 * it back without a board.
 *
 * The protocol is NEC, the one of most cheap remotes: a 9 ms mark, a 4.5 ms
 * space, four bytes bit by bit (the address, its complement, the command and
 * its complement) and a stop mark. A bit is a 560 us mark and a space of
 * 560 us for a 0 or of 1690 us for a 1, least significant bit first. The
 * complement is what makes a damaged frame detectable: a receiver that reads
 * a byte and not its complement heard noise, not a remote.
 */
#ifndef MICRO_OS_DRIVERS_IR_NEC_H
#define MICRO_OS_DRIVERS_IR_NEC_H

#include <stdbool.h>
#include <stdint.h>

/* One pulse of a frame: the carrier on for `mark_us`, then off for
 * `space_us`. The space of the last pulse of a frame is 0. */
struct ir_pulse {
    uint16_t mark_us;
    uint16_t space_us;
};

/* The longest frame the builder writes: a header, 32 bits and a stop. */
#define IR_NEC_PULSES_MAX 34

/* The fields of the frame of a NEC command. */
struct ir_nec_frame {
    uint8_t address;
    uint8_t command;
};

/*
 * The frame of a classic NEC command: the header, the four bytes and the
 * stop mark, as the pulses of the carrier. Returns the number of the pulses
 * written, or 0 when `max` is smaller than IR_NEC_PULSES_MAX.
 */
unsigned ir_nec_build(uint8_t address, uint8_t command, struct ir_pulse *pulses, unsigned max);

/*
 * The other way round: the fields of a frame that is one NEC command within
 * the tolerances of a receiver - a quarter of the nominal length, which is
 * the widest stretch that still tells a 0 from a 1. False for anything else:
 * another protocol, a truncated frame, a byte whose complement disagrees.
 */
bool ir_nec_decode(const struct ir_pulse *pulses, unsigned count, struct ir_nec_frame *frame);

#endif /* MICRO_OS_DRIVERS_IR_NEC_H */
