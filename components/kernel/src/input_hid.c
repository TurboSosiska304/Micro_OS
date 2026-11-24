/*
 * Micro OS — the boot report of a keyboard of the USB (kernel/input_hid.h).
 *
 * The table of the usages is a switch, as the table of the set 1 is: the
 * keyboard page has about a hundred keys and the numbers between them are
 * holes of the specification, so a switch is a table the compiler can check
 * for a duplicated case. The order of the cases follows the layout of a
 * keyboard: the letters, the digits of the top row, the keys a line needs,
 * the punctuation and the block of the cursor at the end.
 *
 * The characters of the console are the US layout of the PS/2 map
 * (ports/port_x86_64/src/ps2.c) and not the layout of the device: a keyboard
 * of another country reports the same usage for the key left of `z`, and this
 * system has no `keymap` yet. Tab and Esc type nothing, as they do there.
 */
#include "kernel/input_hid.h"

/* The bit of the modifier mask of a keyboard of the boot protocol. */
#define HID_MOD_LEFT_CTRL 0x01u
#define HID_MOD_LEFT_SHIFT 0x02u
#define HID_MOD_LEFT_ALT 0x04u
#define HID_MOD_RIGHT_CTRL 0x10u
#define HID_MOD_RIGHT_SHIFT 0x20u
#define HID_MOD_RIGHT_ALT 0x40u

/* The usage of Caps Lock: a press of it is a lock and its release is not. */
#define HID_USAGE_CAPS 0x39u

k_u16_t k_input_hid_key(k_u8_t usage)
{
    /* The six letters of the top row down to the bottom, the digits and the
     * function keys are runs of the page without a hole. */
    if (usage >= 0x04u && usage <= 0x1Du) {
        return (k_u16_t)(K_INPUT_KEY_A + (usage - 0x04u));
    }

    if (usage >= 0x1Eu && usage <= 0x26u) {
        return (k_u16_t)(K_INPUT_KEY_1 + (usage - 0x1Eu));
    }

    if (usage >= 0x3Au && usage <= 0x45u) {
        return (k_u16_t)(K_INPUT_KEY_F1 + (usage - 0x3Au));
    }

    switch (usage) {
    case 0x27u:
        return K_INPUT_KEY_0;
    case 0x28u:
        return K_INPUT_KEY_ENTER;
    case 0x29u:
        return K_INPUT_KEY_ESC;
    case 0x2Au:
        return K_INPUT_KEY_BACKSPACE;
    case 0x2Bu:
        return K_INPUT_KEY_TAB;
    case 0x2Cu:
        return K_INPUT_KEY_SPACE;
    case 0x2Du:
        return K_INPUT_KEY_MINUS;
    case 0x2Eu:
        return K_INPUT_KEY_EQUAL;
    case 0x2Fu:
        return K_INPUT_KEY_LEFTBRACE;
    case 0x30u:
        return K_INPUT_KEY_RIGHTBRACE;
    case 0x31u:
        return K_INPUT_KEY_BACKSLASH;
    case 0x33u:
        return K_INPUT_KEY_SEMICOLON;
    case 0x34u:
        return K_INPUT_KEY_APOSTROPHE;
    case 0x35u:
        return K_INPUT_KEY_GRAVE;
    case 0x36u:
        return K_INPUT_KEY_COMMA;
    case 0x37u:
        return K_INPUT_KEY_DOT;
    case 0x38u:
        return K_INPUT_KEY_SLASH;
    case HID_USAGE_CAPS:
        return K_INPUT_KEY_CAPSLOCK;
    case 0x49u:
        return K_INPUT_KEY_INSERT;
    case 0x4Au:
        return K_INPUT_KEY_HOME;
    case 0x4Bu:
        return K_INPUT_KEY_PAGEUP;
    case 0x4Cu:
        return K_INPUT_KEY_DELETE;
    case 0x4Du:
        return K_INPUT_KEY_END;
    case 0x4Eu:
        return K_INPUT_KEY_PAGEDOWN;
    case 0x4Fu:
        return K_INPUT_KEY_RIGHT;
    case 0x50u:
        return K_INPUT_KEY_LEFT;
    case 0x51u:
        return K_INPUT_KEY_DOWN;
    case 0x52u:
        return K_INPUT_KEY_UP;
    default:
        return K_INPUT_KEY_NONE;
    }
}

/* The character of a usage with no shift, or 0 for a key that types none. */
static char hid_plain(k_u8_t usage)
{
    if (usage >= 0x04u && usage <= 0x1Du) {
        return (char)('a' + (usage - 0x04u));
    }

    if (usage >= 0x1Eu && usage <= 0x26u) {
        return (char)('1' + (usage - 0x1Eu));
    }

    switch (usage) {
    case 0x27u:
        return '0';
    case 0x28u:
        return '\r';
    case 0x2Au:
        return '\b';
    case 0x2Cu:
        return ' ';
    case 0x2Du:
        return '-';
    case 0x2Eu:
        return '=';
    case 0x2Fu:
        return '[';
    case 0x30u:
        return ']';
    case 0x31u:
        return '\\';
    case 0x33u:
        return ';';
    case 0x34u:
        return '\'';
    case 0x35u:
        return '`';
    case 0x36u:
        return ',';
    case 0x37u:
        return '.';
    case 0x38u:
        return '/';
    default:
        /* A modifier, Tab, Esc, a function key, an arrow: a key of the
         * machine with no character on this console. */
        return 0;
    }
}

/* The character of the shifted key, or 0 when the unshifted one is the
 * character of both (a letter, whose case Caps Lock decides). */
static char hid_shifted(k_u8_t usage)
{
    if (usage >= 0x1Eu && usage <= 0x27u) {
        static const char row[] = "!@#$%^&*()";

        return row[usage - 0x1Eu];
    }

    switch (usage) {
    case 0x2Du:
        return '_';
    case 0x2Eu:
        return '+';
    case 0x2Fu:
        return '{';
    case 0x30u:
        return '}';
    case 0x31u:
        return '|';
    case 0x33u:
        return ':';
    case 0x34u:
        return '"';
    case 0x35u:
        return '~';
    case 0x36u:
        return '<';
    case 0x37u:
        return '>';
    case 0x38u:
        return '?';
    default:
        return 0;
    }
}

/* The character a usage types in the state of the modifiers of this report. */
static char hid_character(const struct k_input_hid_state *state, k_u8_t usage)
{
    char plain = hid_plain(usage);
    char shifted;

    if (plain == 0) {
        return 0;
    }

    if (state->ctrl) {
        /* Ctrl with a letter is the control code of that letter, as it is on
         * the PS/2: Ctrl+C is the one the shell reads as "throw the line
         * away". */
        if (plain >= 'a' && plain <= 'z') {
            return (char)(plain - 'a' + 1);
        }
        return 0;
    }

    if (plain >= 'a' && plain <= 'z') {
        return (state->shift != state->caps) ? (char)(plain - 'a' + 'A') : plain;
    }

    shifted = state->shift ? hid_shifted(usage) : 0;

    return shifted != 0 ? shifted : plain;
}

/* One event of the answer, dropped when the answer is full. The count still
 * moves so that the caller knows how many the report holds. */
static k_size_t hid_emit(struct k_input_hid_event *out, k_size_t max, k_size_t count, k_u16_t key,
                         k_u16_t value, char character, bool repeat)
{
    if (count < max) {
        out[count].key = key;
        out[count].value = value;
        out[count].character = character;
        out[count].repeat = repeat;
    }

    return count + 1u;
}

/* The usage at `slot` is also at another slot of the report: a device of a
 * broken report does not press one key twice. */
static bool hid_duplicate(const k_u8_t *keys, k_u8_t slot)
{
    k_u8_t index;

    for (index = 0; index < slot; index++) {
        if (keys[index] == keys[slot]) {
            return true;
        }
    }

    return false;
}

static bool hid_held(const k_u8_t *keys, k_u8_t usage)
{
    k_u8_t index;

    for (index = 0; index < K_INPUT_HID_KEYS; index++) {
        if (keys[index] == usage) {
            return true;
        }
    }

    return false;
}

void k_input_hid_init(struct k_input_hid_state *state)
{
    k_u8_t index;

    if (state == NULL) {
        return;
    }

    state->modifiers = 0;
    state->shift = false;
    state->ctrl = false;
    state->caps = false;
    state->seen = false;

    for (index = 0; index < K_INPUT_HID_KEYS; index++) {
        state->keys[index] = 0;
    }
}

/*
 * The modifiers, one bit at a time: the left and the right form of a
 * modifier are two usages of the page and one key of the machine, so both
 * of them are an event of K_INPUT_KEY_SHIFT, as they are on the PS/2. The
 * GUI keys are modifiers of the USB and not keys of this contract, and their
 * edits are ignored.
 */
static k_size_t hid_modifiers(struct k_input_hid_state *state, k_u8_t modifiers,
                              struct k_input_hid_event *out, k_size_t max, k_size_t count)
{
    struct {
        k_u8_t bit;
        k_u16_t key;
    } const bits[] = {
        {HID_MOD_LEFT_CTRL, K_INPUT_KEY_CTRL},   {HID_MOD_RIGHT_CTRL, K_INPUT_KEY_CTRL},
        {HID_MOD_LEFT_SHIFT, K_INPUT_KEY_SHIFT}, {HID_MOD_RIGHT_SHIFT, K_INPUT_KEY_SHIFT},
        {HID_MOD_LEFT_ALT, K_INPUT_KEY_ALT},     {HID_MOD_RIGHT_ALT, K_INPUT_KEY_ALT},
    };
    k_u8_t index;

    for (index = 0; index < (k_u8_t)(sizeof(bits) / sizeof(bits[0])); index++) {
        bool was = (state->modifiers & bits[index].bit) != 0u;
        bool now = (modifiers & bits[index].bit) != 0u;

        if (was != now) {
            count = hid_emit(out, max, count, bits[index].key,
                             now ? INPUT_VALUE_PRESS : INPUT_VALUE_RELEASE, 0, false);
        }
    }

    state->modifiers = modifiers;
    state->shift = (modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) != 0u;
    state->ctrl = (modifiers & (HID_MOD_LEFT_CTRL | HID_MOD_RIGHT_CTRL)) != 0u;

    return count;
}

k_size_t k_input_hid_decode(struct k_input_hid_state *state, const k_u8_t *report,
                            struct k_input_hid_event *out, k_size_t max)
{
    k_u8_t keys[K_INPUT_HID_KEYS];
    k_u8_t index;
    k_size_t count = 0;
    bool same = state->seen;
    bool any = false;

    if (state == NULL || report == NULL || out == NULL || max == 0u) {
        return 0;
    }

    for (index = 0; index < K_INPUT_HID_KEYS; index++) {
        keys[index] = report[2u + index];

        if (keys[index] != 0u) {
            any = true;
        }

        if (state->seen && keys[index] != state->keys[index]) {
            same = false;
        }
    }

    if (state->seen && report[0] != state->modifiers) {
        same = false;
    }

    /* The same report again is the typematic repeat of the keys that are
     * still down: the characters come again, the edges do not. */
    if (same && any) {
        for (index = 0; index < K_INPUT_HID_KEYS; index++) {
            k_u16_t key;

            if (keys[index] == 0u || hid_duplicate(keys, index)) {
                continue;
            }

            key = k_input_hid_key(keys[index]);
            if (key == K_INPUT_KEY_NONE) {
                continue;
            }

            count = hid_emit(out, max, count, key, INPUT_VALUE_PRESS,
                             hid_character(state, keys[index]), true);
        }

        return count < max ? count : max;
    }

    count = hid_modifiers(state, report[0], out, max, count);

    /* The releases first: a key that went up cannot be held while another
     * key of the same report types its character. */
    for (index = 0; index < K_INPUT_HID_KEYS; index++) {
        k_u16_t key;

        if (state->keys[index] == 0u || hid_duplicate(state->keys, index) ||
            hid_held(keys, state->keys[index])) {
            continue;
        }

        key = k_input_hid_key(state->keys[index]);
        if (key == K_INPUT_KEY_NONE) {
            continue;
        }

        count = hid_emit(out, max, count, key, INPUT_VALUE_RELEASE, 0, false);
    }

    for (index = 0; index < K_INPUT_HID_KEYS; index++) {
        k_u16_t key;
        char character;

        if (keys[index] == 0u || hid_duplicate(keys, index) || hid_held(state->keys, keys[index])) {
            continue;
        }

        key = k_input_hid_key(keys[index]);

        if (key == K_INPUT_KEY_NONE) {
            continue;
        }

        /* Caps Lock is a lock before it is a character: the keys of the same
         * report are read with the lock it turns on. */
        if (keys[index] == HID_USAGE_CAPS) {
            state->caps = !state->caps;
        }

        character = hid_character(state, keys[index]);
        count = hid_emit(out, max, count, key, INPUT_VALUE_PRESS, character, false);
    }

    for (index = 0; index < K_INPUT_HID_KEYS; index++) {
        state->keys[index] = keys[index];
    }
    state->seen = true;

    return count < max ? count : max;
}
