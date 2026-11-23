/*
 * Micro OS — the keyboard of a program (ADR 0030).
 *
 * `/dev/input0` gives the keys of the machine and not their characters: an
 * event names `key.a`, `key.shift`, `key.ctrl` and whether the key went down
 * or up, and an editor that wants the letter has to keep the state of the
 * modifiers and read the layout itself. That state is the same on every
 * machine with a keyboard of this layout, so it is a table of the userland
 * library and a test on the host holds it (ui/keyboard.h).
 *
 * The layout is the US one, the layout of `ports/port_x86_64/src/ps2.c` and
 * of every machine of this system: the letters, the digits of the top row and
 * the punctuation of a line. Ctrl with a letter is the control code of that
 * letter (Ctrl+S is 0x13), which is how a program reads the combination the
 * kernel reads too. Caps Lock is what it is on a PC: the letters and nothing
 * else.
 */
#ifndef MICRO_OS_UI_KEYBOARD_H
#define MICRO_OS_UI_KEYBOARD_H

#include <stdbool.h>

#include "kernel/input.h"
#include "kernel/types.h"

struct ui_keyboard {
    bool shift;
    bool ctrl;
    bool alt;
    bool caps;
};

/* The state of a keyboard with nothing held and the lock off. */
void ui_keyboard_init(struct ui_keyboard *keyboard);

/*
 * Read one event as the state of the modifiers. The answer is true for an
 * event that is a modifier or the lock - the caller does not treat it as a
 * key - and false for every other event, including the release of a character
 * key. A press and a release of Caps Lock toggle the lock once, on the press.
 */
bool ui_keyboard_event(struct ui_keyboard *keyboard, const struct input_event *event);

/*
 * The character of a key under this state, 0 when the key types nothing
 * (Enter, Tab, the arrows, a modifier) and the control code of a letter when
 * Ctrl is held. The caller reads Enter and Tab itself: they are commands of
 * a program and not characters of a file.
 */
char ui_keyboard_char(const struct ui_keyboard *keyboard, k_u16_t code);

#endif /* MICRO_OS_UI_KEYBOARD_H */
