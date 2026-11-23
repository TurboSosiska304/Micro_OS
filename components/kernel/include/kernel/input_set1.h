/*
 * Micro OS — the scancodes of the keyboard as the keys of the machine.
 *
 * The keyboard of the PC speaks the scancode set 1: one byte for a key that
 * goes down, the same byte with the top bit set for the key that comes up,
 * and the prefix 0xE0 (or 0xE1) before the keys the first 128 addresses do
 * not have. The translation from that byte to the name of the machine is a
 * table of the contract and not a fact of the port: every machine with a
 * keyboard of this family answers the same number, a program reads it in a
 * `struct input_event` and a test on the host can check it without hardware.
 *
 * `scancode` is the make code with the break bit already removed (0..0x7F);
 * `extended` is true for the byte behind an E0 prefix. The port keeps the
 * modifiers, the characters and the rings - this file answers only "which
 * key is this address", which is the half a test can hold.
 */
#ifndef MICRO_OS_KERNEL_INPUT_SET1_H
#define MICRO_OS_KERNEL_INPUT_SET1_H

#include "kernel/input.h"
#include "kernel/types.h"

/*
 * The key of a scancode, or K_INPUT_KEY_NONE when the address is not a key
 * of the contract. An address of the set without the E0 prefix and the same
 * address with it are two different keys: 0x1C is Enter and E0 0x1C is the
 * Enter of the keypad, 0x48 is the 8 of the pad and E0 0x48 is Up.
 */
k_u16_t k_input_set1_key(k_u8_t scancode, bool extended);

#endif /* MICRO_OS_KERNEL_INPUT_SET1_H */
