/*
 * Micro OS — the frame of an infrared remote, as arithmetic.
 *
 * See ir_nec.h for what a frame is and why the numbers live in a file of
 * their own. The times are the ones of the NEC standard; the tolerances of
 * the reader are wide enough for the element of a cheap remote and narrow
 * enough that the header of another protocol does not pass for this one.
 */
#include "drivers/ir_nec.h"

#include <stddef.h>

/* The header and the bit of the standard, in microseconds. */
#define IR_NEC_HEADER_MARK 9000
#define IR_NEC_HEADER_SPACE 4500
#define IR_NEC_BIT_MARK 560
#define IR_NEC_ZERO_SPACE 560
#define IR_NEC_ONE_SPACE 1690
#define IR_NEC_STOP_MARK 560

/*
 * The tolerance of the reader: a quarter of the nominal length. A receiver
 * stretches and shortens the pulses by a few percent; a quarter is what the
 * remotes of the world survive, and a 0 (560 us) and a 1 (1690 us) are three
 * quarters apart, so the two windows never meet.
 */
static bool near(unsigned value, unsigned nominal)
{
    unsigned low = nominal - nominal / 4;
    unsigned high = nominal + nominal / 4;

    return value >= low && value <= high;
}

unsigned ir_nec_build(uint8_t address, uint8_t command, struct ir_pulse *pulses, unsigned max)
{
    uint8_t bytes[4];
    unsigned count = 0;
    unsigned i;

    if (pulses == NULL || max < IR_NEC_PULSES_MAX) {
        return 0;
    }

    bytes[0] = address;
    bytes[1] = (uint8_t)~address;
    bytes[2] = command;
    bytes[3] = (uint8_t)~command;

    pulses[count].mark_us = IR_NEC_HEADER_MARK;
    pulses[count].space_us = IR_NEC_HEADER_SPACE;
    count++;

    for (i = 0; i < 4; i++) {
        int bit;

        /* Least significant bit first, as every NEC remote sends it. */
        for (bit = 0; bit < 8; bit++) {
            pulses[count].mark_us = IR_NEC_BIT_MARK;
            pulses[count].space_us =
                (bytes[i] & (1u << bit)) ? IR_NEC_ONE_SPACE : IR_NEC_ZERO_SPACE;
            count++;
        }
    }

    /* The stop mark: the carrier on, and then nothing. */
    pulses[count].mark_us = IR_NEC_STOP_MARK;
    pulses[count].space_us = 0;
    count++;

    return count;
}

/*
 * The pulses as the four bytes of the payload. The count is checked first:
 * a reader that hands over a burst of noise has no header and no 32 bits.
 */
bool ir_nec_decode(const struct ir_pulse *pulses, unsigned count, struct ir_nec_frame *frame)
{
    uint8_t bytes[4] = {0, 0, 0, 0};
    unsigned i;

    if (pulses == NULL || frame == NULL || count != IR_NEC_PULSES_MAX) {
        return false;
    }

    if (!near(pulses[0].mark_us, IR_NEC_HEADER_MARK) ||
        !near(pulses[0].space_us, IR_NEC_HEADER_SPACE)) {
        return false;
    }

    for (i = 0; i < 32; i++) {
        const struct ir_pulse *pulse = &pulses[1 + i];

        if (!near(pulse->mark_us, IR_NEC_BIT_MARK)) {
            return false;
        }

        if (near(pulse->space_us, IR_NEC_ONE_SPACE)) {
            bytes[i / 8] |= (uint8_t)(1u << (i % 8));
        } else if (!near(pulse->space_us, IR_NEC_ZERO_SPACE)) {
            return false;
        }
    }

    if (!near(pulses[IR_NEC_PULSES_MAX - 1].mark_us, IR_NEC_STOP_MARK)) {
        return false;
    }

    /* The complement of every byte is the check of the protocol: a frame
     * that fails it is noise that happened to have the right rhythm. */
    if ((uint8_t)~bytes[0] != bytes[1] || (uint8_t)~bytes[2] != bytes[3]) {
        return false;
    }

    frame->address = bytes[0];
    frame->command = bytes[2];

    return true;
}
