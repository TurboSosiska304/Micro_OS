/*
 * Micro OS — the scancodes of the keyboard in the sets it can speak, as the
 * bytes of the set 1.
 *
 * A keyboard of the PC can be in one of three sets (kernel/input_scancode.c):
 * the set 1 of the XT, the set 2 of the AT and the set 3 of the PS/2. The
 * controller of the motherboard - the 8042 - translates the set 2 into the
 * set 1 for the programs that were written before the AT existed, and a
 * machine whose firmware leaves that translation on never shows which set its
 * keyboard is really in. A machine that turns it off does: the keyboard of the
 * ThinkPad of 1.8 speaks the set 2, the laptop of QEMU the set 1 (through the
 * translation of its own 8042), and a machine of a BIOS of 1984 the set 1.
 *
 * The console and the events of the machine are written in the set 1
 * (kernel/input_set1.h) and in the layout of the US keyboard, so this file is
 * the one place that knows the other two: the address of a key of a set, the
 * same key as the address of the set 1. Two things are pure and a test on the
 * host holds them: which key an address is, and what a stream of bytes of a
 * set means (the make and the break of a key, the prefix of the extended
 * block, the sequence of the Pause).
 */
#ifndef MICRO_OS_KERNEL_INPUT_SCANCODE_H
#define MICRO_OS_KERNEL_INPUT_SCANCODE_H

#include "kernel/input.h"
#include "kernel/types.h"

/* The sets of the specification, by the number a keyboard answers to the
 * command that asks it which set it speaks (0xF0 0x00). */
#define K_INPUT_SCANCODE_SET1 1u
#define K_INPUT_SCANCODE_SET2 2u
#define K_INPUT_SCANCODE_SET3 3u

/* One key of the machine, as the port of the console reads it. */
struct k_input_code {
    k_u8_t code;   /* the address of the key in the set 1 (0: no key) */
    bool extended; /* it is the key behind the prefix 0xE0 of the set 1 */
    bool released; /* the key went up */
};

/*
 * The state of one stream of bytes of a set: the prefix 0xE0 is waiting for
 * its code, the prefix 0xF0 of the sets 2 and 3 for the key that goes up, and
 * the sequence of the Pause is counting its bytes out.
 */
struct k_input_decoder {
    k_u8_t set;   /* one of K_INPUT_SCANCODE_SET* */
    k_u8_t pause; /* the bytes of the sequence of the Pause still to drop */
    bool extended;
    bool released;
};

/*
 * Prepare a decoder for a set. A number that is not one of the three is read
 * as the set 1: a machine whose keyboard did not say which set it speaks is a
 * machine of the XT or of the translation of the 8042.
 */
void k_input_decoder_init(struct k_input_decoder *decoder, k_u8_t set);

/*
 * One byte of the keyboard. True when the byte completed a key of the
 * machine, and `out` then holds the address of that key in the set 1 with its
 * edge; false for a prefix, for a byte of the sequence of the Pause, and for
 * an address no key of this console has. A byte that says nothing leaves the
 * state of the decoder as it was, so a stream of rubbish does not cost the
 * keys that follow it.
 */
bool k_input_decoder_feed(struct k_input_decoder *decoder, k_u8_t byte, struct k_input_code *out);

/*
 * The same key, asked of one address alone: the address of a set - the make
 * code, with `extended` for the keys behind the prefix 0xE0 of the set 2 -
 * as the address of the set 1. False when the address is not a key of this
 * console (a multimedia key, a prefix, a byte that is not an address at all).
 *
 * The set 1 answers itself, including the addresses the layout of the console
 * types and the events do not name (the digits of the keypad), because a key
 * that types a character is a key of the console even when no event of it
 * exists.
 */
bool k_input_scancode_translate(k_u8_t set, k_u8_t code, bool extended, k_u8_t *set1_code,
                                bool *set1_extended);

#endif /* MICRO_OS_KERNEL_INPUT_SCANCODE_H */
