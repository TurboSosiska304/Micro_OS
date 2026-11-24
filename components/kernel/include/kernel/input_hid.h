/*
 * Micro OS — a boot-protocol report of a keyboard of the USB as the keys of
 * the machine (ADR 0040).
 *
 * A keyboard of the USB reports its state and not its edges: eight bytes of
 * the boot protocol of HID (HID 1.11, appendix B.1), the first of them a mask
 * of the eight modifiers and the other seven a reserved byte and six usages
 * of the keyboard page (0x07) - the keys that are down at this moment, in an
 * order the device chooses. The usages are not the scancodes of the PC: 0x04
 * is `a` and 0x1E is `1`, and the number of a key of the machine is neither.
 *
 * This file is the portable half of the driver: the table of the usages, the
 * state of the modifiers and of Caps Lock, and the difference between one
 * report and the next, which is the only place a press or a release of a key
 * of the USB exists. It knows no controller, no endpoint and no queue, so a
 * host test drives it with the reports of a keyboard of the specification
 * (tests/cases/test_input_hid.c) and the port of the machine is the caller.
 *
 * The comparison is the same that the set 1 makes of a break code: the six
 * slots are a set and not a sequence, the order of two keys in one report is
 * the device's business, and a usage that is still in the six at the next
 * report was not released.
 *
 * A report that is the same as the one before it is the typematic repeat of a
 * held key (a boot keyboard sends the report again while a key is held); the
 * keys are told as `repeat` and the caller types their characters a second
 * time, as the keyboard of the PS/2 repeats its make code.
 */
#ifndef MICRO_OS_KERNEL_INPUT_HID_H
#define MICRO_OS_KERNEL_INPUT_HID_H

#include "kernel/input.h"
#include "kernel/types.h"

/* The eight bytes of the report and the six usages of it. */
#define K_INPUT_HID_REPORT 8u
#define K_INPUT_HID_KEYS 6u

/* The state across two reports: the modifiers, the keys and the locks. */
struct k_input_hid_state {
    k_u8_t modifiers; /* the mask of the report before this one */
    k_u8_t keys[K_INPUT_HID_KEYS];
    bool shift; /* either Shift is down */
    bool ctrl;  /* either Ctrl is down */
    bool caps;  /* Caps Lock is on */
    bool seen;  /* a report has been decoded since the init */
};

/* One edge of a key, with the character it types when it types one. A repeat
 * is a key that is still held and not a new press: the moment is not an edge,
 * and only the character of it is the news. */
struct k_input_hid_event {
    k_u16_t key;    /* one of K_INPUT_KEY_*, K_INPUT_KEY_NONE for no key */
    k_u16_t value;  /* INPUT_VALUE_PRESS or INPUT_VALUE_RELEASE */
    char character; /* the character of the console, 0 for none */
    bool repeat;    /* the typematic echo of a held key */
};

/* The state of a keyboard that has sent nothing yet. */
void k_input_hid_init(struct k_input_hid_state *state);

/* The key of a usage of the page 7, or K_INPUT_KEY_NONE when the usage is not
 * a key of this console. */
k_u16_t k_input_hid_key(k_u8_t usage);

/*
 * The events of one report: the modifiers that changed, the keys that came
 * down and the keys that went up, capped at `max`. The answer is the number
 * written; a report that holds more than `max` events loses the rest and the
 * caller does not lose its state (the report is stored whatever the cap).
 */
k_size_t k_input_hid_decode(struct k_input_hid_state *state, const k_u8_t *report,
                            struct k_input_hid_event *out, k_size_t max);

#endif /* MICRO_OS_KERNEL_INPUT_HID_H */
