/*
 * Micro OS — the keys of the machine as events (ADR 0028).
 *
 * `/dev/input0` gives a program the keyboard as a stream of events: a moment,
 * the name of the key and whether it went down or up. The console of the
 * kernel gives characters and nothing else, which is what a shell needs and
 * what a program with a focus cannot use - `Tab` and `Enter` are the same
 * byte as a control sequence there, and a release does not exist at all.
 *
 * The names of the keys are the numbers of the machine. They are part of the
 * ABI of the device and are never reused or reordered: `key.a` is one number
 * for every program of the system and every machine.
 */
#ifndef MICRO_OS_KERNEL_INPUT_H
#define MICRO_OS_KERNEL_INPUT_H

#include "kernel/types.h"

/* One event of the keyboard. Eight bytes on the wire, no padding. */
struct input_event {
    k_u32_t ms;    /* milliseconds since the boot */
    k_u16_t code;  /* one of the K_INPUT_KEY_* names below */
    k_u16_t value; /* 1: pressed, 0: released */
};

#define INPUT_VALUE_RELEASE 0u
#define INPUT_VALUE_PRESS 1u

/* The keys of the machine, in the order of the contract. */
enum k_input_key {
    K_INPUT_KEY_NONE = 0,
    /* The letters, in the order of the alphabet. */
    K_INPUT_KEY_A = 1,
    K_INPUT_KEY_B,
    K_INPUT_KEY_C,
    K_INPUT_KEY_D,
    K_INPUT_KEY_E,
    K_INPUT_KEY_F,
    K_INPUT_KEY_G,
    K_INPUT_KEY_H,
    K_INPUT_KEY_I,
    K_INPUT_KEY_J,
    K_INPUT_KEY_K,
    K_INPUT_KEY_L,
    K_INPUT_KEY_M,
    K_INPUT_KEY_N,
    K_INPUT_KEY_O,
    K_INPUT_KEY_P,
    K_INPUT_KEY_Q,
    K_INPUT_KEY_R,
    K_INPUT_KEY_S,
    K_INPUT_KEY_T,
    K_INPUT_KEY_U,
    K_INPUT_KEY_V,
    K_INPUT_KEY_W,
    K_INPUT_KEY_X,
    K_INPUT_KEY_Y,
    K_INPUT_KEY_Z,
    /* The digits of the top row. */
    K_INPUT_KEY_0,
    K_INPUT_KEY_1,
    K_INPUT_KEY_2,
    K_INPUT_KEY_3,
    K_INPUT_KEY_4,
    K_INPUT_KEY_5,
    K_INPUT_KEY_6,
    K_INPUT_KEY_7,
    K_INPUT_KEY_8,
    K_INPUT_KEY_9,
    /* The keys a line and an interface need. */
    K_INPUT_KEY_ENTER,
    K_INPUT_KEY_ESC,
    K_INPUT_KEY_TAB,
    K_INPUT_KEY_BACKSPACE,
    K_INPUT_KEY_SPACE,
    K_INPUT_KEY_MINUS,
    K_INPUT_KEY_EQUAL,
    K_INPUT_KEY_LEFTBRACE,
    K_INPUT_KEY_RIGHTBRACE,
    K_INPUT_KEY_BACKSLASH,
    K_INPUT_KEY_SEMICOLON,
    K_INPUT_KEY_APOSTROPHE,
    K_INPUT_KEY_GRAVE,
    K_INPUT_KEY_COMMA,
    K_INPUT_KEY_DOT,
    K_INPUT_KEY_SLASH,
    /* The modifiers, as keys of their own. */
    K_INPUT_KEY_CAPSLOCK,
    K_INPUT_KEY_SHIFT,
    K_INPUT_KEY_CTRL,
    K_INPUT_KEY_ALT,
    /* The function keys. */
    K_INPUT_KEY_F1,
    K_INPUT_KEY_F2,
    K_INPUT_KEY_F3,
    K_INPUT_KEY_F4,
    K_INPUT_KEY_F5,
    K_INPUT_KEY_F6,
    K_INPUT_KEY_F7,
    K_INPUT_KEY_F8,
    K_INPUT_KEY_F9,
    K_INPUT_KEY_F10,
    K_INPUT_KEY_F11,
    K_INPUT_KEY_F12,
    K_INPUT_KEY_COUNT
};

/* The name of a key of the machine: "key.enter", "key.f1", "key.a"; "key.none"
 * for a number that is not one of the table. */
const char *k_input_key_name(k_u16_t code);

#endif /* MICRO_OS_KERNEL_INPUT_H */
