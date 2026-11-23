/*
 * Micro OS — the table of the scancode set 1 (kernel/input_set1.h).
 *
 * The table is a switch and not an array of 128 entries: the addresses are
 * the rows of a PC keyboard, most of the 256 numbers are not keys of this
 * console, and a switch is a table the compiler can check for a duplicated
 * case. The order of the cases follows the layout of the keyboard: the
 * letters of the top, the middle and the bottom row, the digits, the keys a
 * line needs, the modifiers and the function keys; the extended block is the
 * second form of the keys the pad and the arrows use.
 *
 * A key that types nothing - a modifier, a function key - is still a key of
 * the machine and has its number here: a program that draws a help line with
 * "F1" is a program that reads the event of F1.
 */
#include "kernel/input_set1.h"

k_u16_t k_input_set1_key(k_u8_t scancode, bool extended)
{
    if (extended) {
        switch (scancode) {
        case 0x48u:
            return K_INPUT_KEY_UP;
        case 0x50u:
            return K_INPUT_KEY_DOWN;
        case 0x4Bu:
            return K_INPUT_KEY_LEFT;
        case 0x4Du:
            return K_INPUT_KEY_RIGHT;
        case 0x47u:
            return K_INPUT_KEY_HOME;
        case 0x4Fu:
            return K_INPUT_KEY_END;
        case 0x49u:
            return K_INPUT_KEY_PAGEUP;
        case 0x51u:
            return K_INPUT_KEY_PAGEDOWN;
        case 0x52u:
            return K_INPUT_KEY_INSERT;
        case 0x53u:
            return K_INPUT_KEY_DELETE;
        /* The pad of the second form: the same keys as the main block, and
         * the right-hand modifiers. */
        case 0x1Cu:
            return K_INPUT_KEY_ENTER;
        case 0x35u:
            return K_INPUT_KEY_SLASH;
        case 0x1Du:
            return K_INPUT_KEY_CTRL;
        case 0x38u:
            return K_INPUT_KEY_ALT;
        default:
            return K_INPUT_KEY_NONE;
        }
    }

    switch (scancode) {
    case 0x1Eu:
        return K_INPUT_KEY_A;
    case 0x30u:
        return K_INPUT_KEY_B;
    case 0x2Eu:
        return K_INPUT_KEY_C;
    case 0x20u:
        return K_INPUT_KEY_D;
    case 0x12u:
        return K_INPUT_KEY_E;
    case 0x21u:
        return K_INPUT_KEY_F;
    case 0x22u:
        return K_INPUT_KEY_G;
    case 0x23u:
        return K_INPUT_KEY_H;
    case 0x17u:
        return K_INPUT_KEY_I;
    case 0x24u:
        return K_INPUT_KEY_J;
    case 0x25u:
        return K_INPUT_KEY_K;
    case 0x26u:
        return K_INPUT_KEY_L;
    case 0x32u:
        return K_INPUT_KEY_M;
    case 0x31u:
        return K_INPUT_KEY_N;
    case 0x18u:
        return K_INPUT_KEY_O;
    case 0x19u:
        return K_INPUT_KEY_P;
    case 0x10u:
        return K_INPUT_KEY_Q;
    case 0x13u:
        return K_INPUT_KEY_R;
    case 0x1Fu:
        return K_INPUT_KEY_S;
    case 0x14u:
        return K_INPUT_KEY_T;
    case 0x16u:
        return K_INPUT_KEY_U;
    case 0x2Fu:
        return K_INPUT_KEY_V;
    case 0x11u:
        return K_INPUT_KEY_W;
    case 0x2Du:
        return K_INPUT_KEY_X;
    case 0x15u:
        return K_INPUT_KEY_Y;
    case 0x2Cu:
        return K_INPUT_KEY_Z;
    case 0x0Bu:
        return K_INPUT_KEY_0;
    case 0x02u:
        return K_INPUT_KEY_1;
    case 0x03u:
        return K_INPUT_KEY_2;
    case 0x04u:
        return K_INPUT_KEY_3;
    case 0x05u:
        return K_INPUT_KEY_4;
    case 0x06u:
        return K_INPUT_KEY_5;
    case 0x07u:
        return K_INPUT_KEY_6;
    case 0x08u:
        return K_INPUT_KEY_7;
    case 0x09u:
        return K_INPUT_KEY_8;
    case 0x0Au:
        return K_INPUT_KEY_9;
    case 0x1Cu:
        return K_INPUT_KEY_ENTER;
    case 0x01u:
        return K_INPUT_KEY_ESC;
    case 0x0Fu:
        return K_INPUT_KEY_TAB;
    case 0x0Eu:
        return K_INPUT_KEY_BACKSPACE;
    case 0x39u:
        return K_INPUT_KEY_SPACE;
    case 0x0Cu:
        return K_INPUT_KEY_MINUS;
    case 0x0Du:
        return K_INPUT_KEY_EQUAL;
    case 0x1Au:
        return K_INPUT_KEY_LEFTBRACE;
    case 0x1Bu:
        return K_INPUT_KEY_RIGHTBRACE;
    case 0x2Bu:
        return K_INPUT_KEY_BACKSLASH;
    case 0x27u:
        return K_INPUT_KEY_SEMICOLON;
    case 0x28u:
        return K_INPUT_KEY_APOSTROPHE;
    case 0x29u:
        return K_INPUT_KEY_GRAVE;
    case 0x33u:
        return K_INPUT_KEY_COMMA;
    case 0x34u:
        return K_INPUT_KEY_DOT;
    case 0x35u:
        return K_INPUT_KEY_SLASH;
    case 0x3Au:
        return K_INPUT_KEY_CAPSLOCK;
    case 0x2Au: /* left shift */
    case 0x36u: /* right shift */
        return K_INPUT_KEY_SHIFT;
    case 0x1Du:
        return K_INPUT_KEY_CTRL;
    case 0x38u:
        return K_INPUT_KEY_ALT;
    case 0x3Bu:
        return K_INPUT_KEY_F1;
    case 0x3Cu:
        return K_INPUT_KEY_F2;
    case 0x3Du:
        return K_INPUT_KEY_F3;
    case 0x3Eu:
        return K_INPUT_KEY_F4;
    case 0x3Fu:
        return K_INPUT_KEY_F5;
    case 0x40u:
        return K_INPUT_KEY_F6;
    case 0x41u:
        return K_INPUT_KEY_F7;
    case 0x42u:
        return K_INPUT_KEY_F8;
    case 0x43u:
        return K_INPUT_KEY_F9;
    case 0x44u:
        return K_INPUT_KEY_F10;
    case 0x57u:
        return K_INPUT_KEY_F11;
    case 0x58u:
        return K_INPUT_KEY_F12;
    /* The numbers of the keypad (0x47..0x53 without the prefix) are not keys
     * of this console: the pad is the second form of the arrows and of Home,
     * End, Insert and Delete, and those are the extended block above. */
    default:
        return K_INPUT_KEY_NONE;
    }
}
