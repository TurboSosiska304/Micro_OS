/*
 * Micro OS — the layout of the keyboard as a program sees it (ui/keyboard.h).
 *
 * The tables are the two of the port (the plain one and the shifted one), in
 * another shape: the port maps a scancode of the set 1, this file maps the
 * name the event carries, and the two meet at the character a person meant.
 * The letters are one range of the enum (`key.a` .. `key.z`), so the letter
 * is arithmetic; the punctuation is a switch because its addresses are not
 * in an order a compiler could trust.
 */
#include "ui/keyboard.h"

void ui_keyboard_init(struct ui_keyboard *keyboard)
{
    if (keyboard == NULL) {
        return;
    }

    keyboard->shift = false;
    keyboard->ctrl = false;
    keyboard->alt = false;
    keyboard->caps = false;
}

bool ui_keyboard_event(struct ui_keyboard *keyboard, const struct input_event *event)
{
    bool down;

    if (keyboard == NULL || event == NULL) {
        return false;
    }

    down = event->value == INPUT_VALUE_PRESS;

    switch (event->code) {
    case K_INPUT_KEY_SHIFT:
        keyboard->shift = down;
        return true;
    case K_INPUT_KEY_CTRL:
        keyboard->ctrl = down;
        return true;
    case K_INPUT_KEY_ALT:
        keyboard->alt = down;
        return true;
    case K_INPUT_KEY_CAPSLOCK:
        if (down) {
            keyboard->caps = !keyboard->caps;
        }
        return true;
    default:
        return false;
    }
}

/* The character of a key without the shift, 0 for a key that is not one of
 * the printable set. */
static char plain_char(k_u16_t code)
{
    switch (code) {
    case K_INPUT_KEY_0:
        return '0';
    case K_INPUT_KEY_1:
        return '1';
    case K_INPUT_KEY_2:
        return '2';
    case K_INPUT_KEY_3:
        return '3';
    case K_INPUT_KEY_4:
        return '4';
    case K_INPUT_KEY_5:
        return '5';
    case K_INPUT_KEY_6:
        return '6';
    case K_INPUT_KEY_7:
        return '7';
    case K_INPUT_KEY_8:
        return '8';
    case K_INPUT_KEY_9:
        return '9';
    case K_INPUT_KEY_SPACE:
        return ' ';
    case K_INPUT_KEY_MINUS:
        return '-';
    case K_INPUT_KEY_EQUAL:
        return '=';
    case K_INPUT_KEY_LEFTBRACE:
        return '[';
    case K_INPUT_KEY_RIGHTBRACE:
        return ']';
    case K_INPUT_KEY_BACKSLASH:
        return '\\';
    case K_INPUT_KEY_SEMICOLON:
        return ';';
    case K_INPUT_KEY_APOSTROPHE:
        return '\'';
    case K_INPUT_KEY_GRAVE:
        return '`';
    case K_INPUT_KEY_COMMA:
        return ',';
    case K_INPUT_KEY_DOT:
        return '.';
    case K_INPUT_KEY_SLASH:
        return '/';
    default:
        return 0;
    }
}

/* The same key with the shift held, 0 when the shift changes nothing (the
 * digits of a keypad, a key that is not printable). */
static char shifted_char(k_u16_t code)
{
    switch (code) {
    case K_INPUT_KEY_0:
        return ')';
    case K_INPUT_KEY_1:
        return '!';
    case K_INPUT_KEY_2:
        return '@';
    case K_INPUT_KEY_3:
        return '#';
    case K_INPUT_KEY_4:
        return '$';
    case K_INPUT_KEY_5:
        return '%';
    case K_INPUT_KEY_6:
        return '^';
    case K_INPUT_KEY_7:
        return '&';
    case K_INPUT_KEY_8:
        return '*';
    case K_INPUT_KEY_9:
        return '(';
    case K_INPUT_KEY_MINUS:
        return '_';
    case K_INPUT_KEY_EQUAL:
        return '+';
    case K_INPUT_KEY_LEFTBRACE:
        return '{';
    case K_INPUT_KEY_RIGHTBRACE:
        return '}';
    case K_INPUT_KEY_BACKSLASH:
        return '|';
    case K_INPUT_KEY_SEMICOLON:
        return ':';
    case K_INPUT_KEY_APOSTROPHE:
        return '"';
    case K_INPUT_KEY_GRAVE:
        return '~';
    case K_INPUT_KEY_COMMA:
        return '<';
    case K_INPUT_KEY_DOT:
        return '>';
    case K_INPUT_KEY_SLASH:
        return '?';
    default:
        return 0;
    }
}

char ui_keyboard_char(const struct ui_keyboard *keyboard, k_u16_t code)
{
    struct ui_keyboard state;
    char plain;

    if (keyboard == NULL) {
        return 0;
    }

    state = *keyboard;

    if (code >= K_INPUT_KEY_A && code <= K_INPUT_KEY_Z) {
        char letter = (char)('a' + (int)(code - K_INPUT_KEY_A));

        /* Ctrl with a letter is the control code, as the console reads it:
         * the shift does not change that combination. */
        if (state.ctrl) {
            return (char)(letter - 'a' + 1);
        }

        if (state.shift != state.caps) {
            return (char)(letter - 'a' + 'A');
        }

        return letter;
    }

    plain = plain_char(code);
    if (plain == 0) {
        return 0;
    }

    if (state.ctrl) {
        return 0; /* Ctrl with a punctuation key is not a character here */
    }

    if (state.shift) {
        char shifted = shifted_char(code);

        if (shifted != 0) {
            return shifted;
        }
    }

    return plain;
}
