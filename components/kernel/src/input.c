/*
 * Micro OS — the names of the keys of the machine (ADR 0028).
 *
 * One table for the whole system: a program reads a `struct input_event` and
 * a person reads the name of the key in a log, and neither has a second copy
 * of the list. The numbers are the contract; the names are how they are
 * written.
 */
#include "kernel/input.h"

static const char *const s_names[K_INPUT_KEY_COUNT] = {
    "key.none",       "key.a",         "key.b",         "key.c",          "key.d",
    "key.e",          "key.f",         "key.g",         "key.h",          "key.i",
    "key.j",          "key.k",         "key.l",         "key.m",          "key.n",
    "key.o",          "key.p",         "key.q",         "key.r",          "key.s",
    "key.t",          "key.u",         "key.v",         "key.w",          "key.x",
    "key.y",          "key.z",         "key.0",         "key.1",          "key.2",
    "key.3",          "key.4",         "key.5",         "key.6",          "key.7",
    "key.8",          "key.9",         "key.enter",     "key.esc",        "key.tab",
    "key.backspace",  "key.space",     "key.minus",     "key.equal",      "key.leftbrace",
    "key.rightbrace", "key.backslash", "key.semicolon", "key.apostrophe", "key.grave",
    "key.comma",      "key.dot",       "key.slash",     "key.capslock",   "key.shift",
    "key.ctrl",       "key.alt",       "key.f1",        "key.f2",         "key.f3",
    "key.f4",         "key.f5",        "key.f6",        "key.f7",         "key.f8",
    "key.f9",         "key.f10",       "key.f11",       "key.f12",
};

const char *k_input_key_name(k_u16_t code)
{
    if (code >= K_INPUT_KEY_COUNT) {
        return s_names[K_INPUT_KEY_NONE];
    }

    return s_names[code];
}
