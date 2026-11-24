/*
 * Micro OS — the sets 2 and 3 of the scancodes as the set 1 (kernel/input_scancode.h).
 *
 * The two tables below are the addresses of the same keyboard, key by key:
 * the set 2 of the AT - the one a laptop of the 2010s still speaks when the
 * firmware of its controller does not translate it - and the set 3 of the
 * PS/2. The reference is the table of the three sets of the PC, the one the
 * keyboard-internal codes of a machine of this family are written in: for
 * every keycap an address of the set 1, of the set 2 and of the set 3, and the
 * address the 8042 makes of the last two when its translation is on.
 *
 * The tables are switches for the reason the table of the set 1 is one: the
 * addresses are the rows of a keyboard, most of the 256 numbers are not keys,
 * and a switch is a table the compiler checks for a case written twice.
 *
 * The set 3 is the regular one of the specification and the least used: most
 * of its keys do not even send a break code unless the controller asks for
 * one, and the keys the set 1 carries behind the prefix 0xE0 have an address
 * of their own here. A keyboard that answers 3 is a museum piece and a
 * workstation of the 1990s; the table is here because the answer of the
 * keyboard at the boot is one byte and a machine that says 3 deserves to be
 * typed at.
 *
 * The keys that no console of this system can use - the Windows keys, the
 * multimedia block of a modern keyboard, the power of a laptop - are not in
 * the tables: their addresses answer "no key", and the decoder drops them.
 * The keyboard of the machine says one code for the Pause and one for the
 * Ctrl+Break of the set 3, and neither is a key of this console.
 */
#include "kernel/input_scancode.h"

/* The keys behind the prefix 0xE0 of the set 2, as the addresses of the set 1:
 * the arrows and the block above them, the right-hand modifiers and the two
 * keys of the keypad the set 1 keeps on the second form. */
static bool set2_extended(k_u8_t code, k_u8_t *set1)
{
    switch (code) {
    case 0x70u:
        *set1 = 0x52u; /* Insert */
        return true;
    case 0x71u:
        *set1 = 0x53u; /* Delete */
        return true;
    case 0x6Cu:
        *set1 = 0x47u; /* Home */
        return true;
    case 0x69u:
        *set1 = 0x4Fu; /* End */
        return true;
    case 0x7Du:
        *set1 = 0x49u; /* Page Up */
        return true;
    case 0x7Au:
        *set1 = 0x51u; /* Page Down */
        return true;
    case 0x6Bu:
        *set1 = 0x4Bu; /* Left */
        return true;
    case 0x75u:
        *set1 = 0x48u; /* Up */
        return true;
    case 0x72u:
        *set1 = 0x50u; /* Down */
        return true;
    case 0x74u:
        *set1 = 0x4Du; /* Right */
        return true;
    case 0x14u:
        *set1 = 0x1Du; /* the Ctrl of the right */
        return true;
    case 0x11u:
        *set1 = 0x38u; /* the Alt of the right */
        return true;
    case 0x5Au:
        *set1 = 0x1Cu; /* the Enter of the keypad */
        return true;
    case 0x4Au:
        *set1 = 0x35u; /* the slash of the keypad */
        return true;
    case 0x7Cu:
        *set1 = 0x37u; /* Print Screen */
        return true;
    case 0x7Eu:
        *set1 = 0x46u; /* Ctrl+Break */
        return true;
    default:
        return false;
    }
}

/* The set 2 of the AT. */
static bool translate_set2(k_u8_t code, bool extended, k_u8_t *set1, bool *extended1)
{
    *extended1 = false;

    if (extended) {
        if (!set2_extended(code, set1)) {
            return false;
        }

        *extended1 = true;
        return true;
    }

    switch (code) {
    /* The letters of the three rows. */
    case 0x1Cu:
        *set1 = 0x1Eu; /* A */
        return true;
    case 0x32u:
        *set1 = 0x30u; /* B */
        return true;
    case 0x21u:
        *set1 = 0x2Eu; /* C */
        return true;
    case 0x23u:
        *set1 = 0x20u; /* D */
        return true;
    case 0x24u:
        *set1 = 0x12u; /* E */
        return true;
    case 0x2Bu:
        *set1 = 0x21u; /* F */
        return true;
    case 0x34u:
        *set1 = 0x22u; /* G */
        return true;
    case 0x33u:
        *set1 = 0x23u; /* H */
        return true;
    case 0x43u:
        *set1 = 0x17u; /* I */
        return true;
    case 0x3Bu:
        *set1 = 0x24u; /* J */
        return true;
    case 0x42u:
        *set1 = 0x25u; /* K */
        return true;
    case 0x4Bu:
        *set1 = 0x26u; /* L */
        return true;
    case 0x3Au:
        *set1 = 0x32u; /* M */
        return true;
    case 0x31u:
        *set1 = 0x31u; /* N */
        return true;
    case 0x44u:
        *set1 = 0x18u; /* O */
        return true;
    case 0x4Du:
        *set1 = 0x19u; /* P */
        return true;
    case 0x15u:
        *set1 = 0x10u; /* Q */
        return true;
    case 0x2Du:
        *set1 = 0x13u; /* R */
        return true;
    case 0x1Bu:
        *set1 = 0x1Fu; /* S */
        return true;
    case 0x2Cu:
        *set1 = 0x14u; /* T */
        return true;
    case 0x3Cu:
        *set1 = 0x16u; /* U */
        return true;
    case 0x2Au:
        *set1 = 0x2Fu; /* V */
        return true;
    case 0x1Du:
        *set1 = 0x11u; /* W */
        return true;
    case 0x22u:
        *set1 = 0x2Du; /* X */
        return true;
    case 0x35u:
        *set1 = 0x15u; /* Y */
        return true;
    case 0x1Au:
        *set1 = 0x2Cu; /* Z */
        return true;
    /* The digits of the top row. */
    case 0x16u:
        *set1 = 0x02u; /* 1 */
        return true;
    case 0x1Eu:
        *set1 = 0x03u; /* 2 */
        return true;
    case 0x26u:
        *set1 = 0x04u; /* 3 */
        return true;
    case 0x25u:
        *set1 = 0x05u; /* 4 */
        return true;
    case 0x2Eu:
        *set1 = 0x06u; /* 5 */
        return true;
    case 0x36u:
        *set1 = 0x07u; /* 6 */
        return true;
    case 0x3Du:
        *set1 = 0x08u; /* 7 */
        return true;
    case 0x3Eu:
        *set1 = 0x09u; /* 8 */
        return true;
    case 0x46u:
        *set1 = 0x0Au; /* 9 */
        return true;
    case 0x45u:
        *set1 = 0x0Bu; /* 0 */
        return true;
    /* The punctuation of a command line. */
    case 0x4Eu:
        *set1 = 0x0Cu; /* - _ */
        return true;
    case 0x55u:
        *set1 = 0x0Du; /* = + */
        return true;
    case 0x54u:
        *set1 = 0x1Au; /* [ { */
        return true;
    case 0x5Bu:
        *set1 = 0x1Bu; /* ] } */
        return true;
    case 0x5Du:
        *set1 = 0x2Bu; /* \ | */
        return true;
    case 0x4Cu:
        *set1 = 0x27u; /* ; : */
        return true;
    case 0x52u:
        *set1 = 0x28u; /* ' " */
        return true;
    case 0x0Eu:
        *set1 = 0x29u; /* ` ~ */
        return true;
    case 0x41u:
        *set1 = 0x33u; /* , < */
        return true;
    case 0x49u:
        *set1 = 0x34u; /* . > */
        return true;
    case 0x4Au:
        *set1 = 0x35u; /* / ? */
        return true;
    /* The keys a line needs and the modifiers. */
    case 0x76u:
        *set1 = 0x01u; /* Esc */
        return true;
    case 0x66u:
        *set1 = 0x0Eu; /* Backspace */
        return true;
    case 0x0Du:
        *set1 = 0x0Fu; /* Tab */
        return true;
    case 0x5Au:
        *set1 = 0x1Cu; /* Enter */
        return true;
    case 0x29u:
        *set1 = 0x39u; /* space */
        return true;
    case 0x12u:
        *set1 = 0x2Au; /* the Shift of the left */
        return true;
    case 0x59u:
        *set1 = 0x36u; /* the Shift of the right */
        return true;
    case 0x14u:
        *set1 = 0x1Du; /* the Ctrl of the left */
        return true;
    case 0x11u:
        *set1 = 0x38u; /* the Alt of the left */
        return true;
    case 0x58u:
        *set1 = 0x3Au; /* Caps Lock */
        return true;
    case 0x77u:
        *set1 = 0x45u; /* Num Lock */
        return true;
    case 0x7Eu:
        *set1 = 0x46u; /* Scroll Lock */
        return true;
    /* The function keys. */
    case 0x05u:
        *set1 = 0x3Bu; /* F1 */
        return true;
    case 0x06u:
        *set1 = 0x3Cu; /* F2 */
        return true;
    case 0x04u:
        *set1 = 0x3Du; /* F3 */
        return true;
    case 0x0Cu:
        *set1 = 0x3Eu; /* F4 */
        return true;
    case 0x03u:
        *set1 = 0x3Fu; /* F5 */
        return true;
    case 0x0Bu:
        *set1 = 0x40u; /* F6 */
        return true;
    case 0x83u:
        *set1 = 0x41u; /* F7 */
        return true;
    case 0x0Au:
        *set1 = 0x42u; /* F8 */
        return true;
    case 0x01u:
        *set1 = 0x43u; /* F9 */
        return true;
    case 0x09u:
        *set1 = 0x44u; /* F10 */
        return true;
    case 0x78u:
        *set1 = 0x57u; /* F11 */
        return true;
    case 0x07u:
        *set1 = 0x58u; /* F12 */
        return true;
    /* The keypad, whose digits the console types and whose arrows are the
     * addresses of the extended block above. */
    case 0x70u:
        *set1 = 0x52u; /* 0 */
        return true;
    case 0x69u:
        *set1 = 0x4Fu; /* 1 */
        return true;
    case 0x72u:
        *set1 = 0x50u; /* 2 */
        return true;
    case 0x7Au:
        *set1 = 0x51u; /* 3 */
        return true;
    case 0x6Bu:
        *set1 = 0x4Bu; /* 4 */
        return true;
    case 0x73u:
        *set1 = 0x4Cu; /* 5 */
        return true;
    case 0x74u:
        *set1 = 0x4Du; /* 6 */
        return true;
    case 0x6Cu:
        *set1 = 0x47u; /* 7 */
        return true;
    case 0x75u:
        *set1 = 0x48u; /* 8 */
        return true;
    case 0x7Du:
        *set1 = 0x49u; /* 9 */
        return true;
    case 0x71u:
        *set1 = 0x53u; /* . */
        return true;
    case 0x7Cu:
        *set1 = 0x37u; /* * */
        return true;
    case 0x7Bu:
        *set1 = 0x4Au; /* - */
        return true;
    case 0x79u:
        *set1 = 0x4Eu; /* + */
        return true;
    case 0x84u:
        *set1 = 0x54u; /* Alt+SysRq */
        return true;
    default:
        return false;
    }
}

/* The set 3 of the PS/2: the letters, the digits and the punctuation are the
 * addresses of the set 2, and the rest of the keyboard is its own - the keys
 * the set 1 carries behind 0xE0 have an address of a byte here, the function
 * keys have their own, and three modifiers of the set 1 moved. */
static bool translate_set3(k_u8_t code, bool extended, k_u8_t *set1, bool *extended1)
{
    *extended1 = false;

    /* The keyboard of the set 3 sends no prefix for the keys the set 1 has
     * behind 0xE0. A keyboard that sends one is a keyboard of the second half
     * of the specification - the Windows and multimedia block - and none of
     * those keys is a key of this console. */
    if (extended) {
        return false;
    }

    switch (code) {
    /* The keys the sets 2 and 3 share, and the three the set 3 moved. */
    case 0x11u:
        *set1 = 0x1Du; /* the Ctrl of the left */
        return true;
    case 0x19u:
        *set1 = 0x38u; /* the Alt of the left */
        return true;
    case 0x14u:
        *set1 = 0x3Au; /* Caps Lock */
        return true;
    case 0x08u:
        *set1 = 0x01u; /* Esc */
        return true;
    case 0x5Cu:
        *set1 = 0x2Bu; /* \ | */
        return true;
    /* The rest of the block of the letters and the digits, and the Shift, the
     * Enter, the Tab, the Space and the Backspace: the address of the set 2. */
    case 0x1Cu:
        *set1 = 0x1Eu; /* A */
        return true;
    case 0x32u:
        *set1 = 0x30u; /* B */
        return true;
    case 0x21u:
        *set1 = 0x2Eu; /* C */
        return true;
    case 0x23u:
        *set1 = 0x20u; /* D */
        return true;
    case 0x24u:
        *set1 = 0x12u; /* E */
        return true;
    case 0x2Bu:
        *set1 = 0x21u; /* F */
        return true;
    case 0x34u:
        *set1 = 0x22u; /* G */
        return true;
    case 0x33u:
        *set1 = 0x23u; /* H */
        return true;
    case 0x43u:
        *set1 = 0x17u; /* I */
        return true;
    case 0x3Bu:
        *set1 = 0x24u; /* J */
        return true;
    case 0x42u:
        *set1 = 0x25u; /* K */
        return true;
    case 0x4Bu:
        *set1 = 0x26u; /* L */
        return true;
    case 0x3Au:
        *set1 = 0x32u; /* M */
        return true;
    case 0x31u:
        *set1 = 0x31u; /* N */
        return true;
    case 0x44u:
        *set1 = 0x18u; /* O */
        return true;
    case 0x4Du:
        *set1 = 0x19u; /* P */
        return true;
    case 0x15u:
        *set1 = 0x10u; /* Q */
        return true;
    case 0x2Du:
        *set1 = 0x13u; /* R */
        return true;
    case 0x1Bu:
        *set1 = 0x1Fu; /* S */
        return true;
    case 0x2Cu:
        *set1 = 0x14u; /* T */
        return true;
    case 0x3Cu:
        *set1 = 0x16u; /* U */
        return true;
    case 0x2Au:
        *set1 = 0x2Fu; /* V */
        return true;
    case 0x1Du:
        *set1 = 0x11u; /* W */
        return true;
    case 0x22u:
        *set1 = 0x2Du; /* X */
        return true;
    case 0x35u:
        *set1 = 0x15u; /* Y */
        return true;
    case 0x1Au:
        *set1 = 0x2Cu; /* Z */
        return true;
    case 0x16u:
        *set1 = 0x02u; /* 1 */
        return true;
    case 0x1Eu:
        *set1 = 0x03u; /* 2 */
        return true;
    case 0x26u:
        *set1 = 0x04u; /* 3 */
        return true;
    case 0x25u:
        *set1 = 0x05u; /* 4 */
        return true;
    case 0x2Eu:
        *set1 = 0x06u; /* 5 */
        return true;
    case 0x36u:
        *set1 = 0x07u; /* 6 */
        return true;
    case 0x3Du:
        *set1 = 0x08u; /* 7 */
        return true;
    case 0x3Eu:
        *set1 = 0x09u; /* 8 */
        return true;
    case 0x46u:
        *set1 = 0x0Au; /* 9 */
        return true;
    case 0x45u:
        *set1 = 0x0Bu; /* 0 */
        return true;
    case 0x4Eu:
        *set1 = 0x0Cu; /* - _ */
        return true;
    case 0x55u:
        *set1 = 0x0Du; /* = + */
        return true;
    case 0x54u:
        *set1 = 0x1Au; /* [ { */
        return true;
    case 0x5Bu:
        *set1 = 0x1Bu; /* ] } */
        return true;
    case 0x4Cu:
        *set1 = 0x27u; /* ; : */
        return true;
    case 0x52u:
        *set1 = 0x28u; /* ' " */
        return true;
    case 0x0Eu:
        *set1 = 0x29u; /* ` ~ */
        return true;
    case 0x41u:
        *set1 = 0x33u; /* , < */
        return true;
    case 0x49u:
        *set1 = 0x34u; /* . > */
        return true;
    case 0x4Au:
        *set1 = 0x35u; /* / ? */
        return true;
    case 0x66u:
        *set1 = 0x0Eu; /* Backspace */
        return true;
    case 0x0Du:
        *set1 = 0x0Fu; /* Tab */
        return true;
    case 0x5Au:
        *set1 = 0x1Cu; /* Enter */
        return true;
    case 0x29u:
        *set1 = 0x39u; /* space */
        return true;
    case 0x12u:
        *set1 = 0x2Au; /* the Shift of the left */
        return true;
    case 0x59u:
        *set1 = 0x36u; /* the Shift of the right */
        return true;
    /* The keys of the cursor block, which the set 3 names with one byte. */
    case 0x67u:
        *set1 = 0x52u;
        *extended1 = true; /* Insert */
        return true;
    case 0x64u:
        *set1 = 0x53u;
        *extended1 = true; /* Delete */
        return true;
    case 0x6Eu:
        *set1 = 0x47u;
        *extended1 = true; /* Home */
        return true;
    case 0x65u:
        *set1 = 0x4Fu;
        *extended1 = true; /* End */
        return true;
    case 0x6Fu:
        *set1 = 0x49u;
        *extended1 = true; /* Page Up */
        return true;
    case 0x6Du:
        *set1 = 0x51u;
        *extended1 = true; /* Page Down */
        return true;
    case 0x61u:
        *set1 = 0x4Bu;
        *extended1 = true; /* Left */
        return true;
    case 0x63u:
        *set1 = 0x48u;
        *extended1 = true; /* Up */
        return true;
    case 0x60u:
        *set1 = 0x50u;
        *extended1 = true; /* Down */
        return true;
    case 0x6Au:
        *set1 = 0x4Du;
        *extended1 = true; /* Right */
        return true;
    case 0x58u:
        *set1 = 0x1Du;
        *extended1 = true; /* the Ctrl of the right */
        return true;
    case 0x39u:
        *set1 = 0x38u;
        *extended1 = true; /* the Alt of the right */
        return true;
    case 0x79u:
        *set1 = 0x1Cu;
        *extended1 = true; /* the Enter of the keypad */
        return true;
    case 0x77u:
        *set1 = 0x35u;
        *extended1 = true; /* the slash of the keypad */
        return true;
    case 0x57u:
        *set1 = 0x37u;
        *extended1 = true; /* Print Screen */
        return true;
    case 0x76u:
        *set1 = 0x45u; /* Num Lock */
        return true;
    case 0x5Fu:
        *set1 = 0x46u; /* Scroll Lock */
        return true;
    /* The keypad: the digits as the set 2 has them, and the four operators and
     * the two keys of the second form, which the set 3 moved. */
    case 0x70u:
        *set1 = 0x52u; /* 0 */
        return true;
    case 0x69u:
        *set1 = 0x4Fu; /* 1 */
        return true;
    case 0x72u:
        *set1 = 0x50u; /* 2 */
        return true;
    case 0x7Au:
        *set1 = 0x51u; /* 3 */
        return true;
    case 0x6Bu:
        *set1 = 0x4Bu; /* 4 */
        return true;
    case 0x73u:
        *set1 = 0x4Cu; /* 5 */
        return true;
    case 0x74u:
        *set1 = 0x4Du; /* 6 */
        return true;
    case 0x6Cu:
        *set1 = 0x47u; /* 7 */
        return true;
    case 0x75u:
        *set1 = 0x48u; /* 8 */
        return true;
    case 0x7Du:
        *set1 = 0x49u; /* 9 */
        return true;
    case 0x71u:
        *set1 = 0x53u; /* . */
        return true;
    case 0x7Eu:
        *set1 = 0x37u; /* * */
        return true;
    case 0x84u:
        *set1 = 0x4Au; /* - */
        return true;
    case 0x7Cu:
        *set1 = 0x4Eu; /* + */
        return true;
    /* The function keys of the set 3, which are not the addresses of the set 2
     * at all. */
    case 0x07u:
        *set1 = 0x3Bu; /* F1 */
        return true;
    case 0x0Fu:
        *set1 = 0x3Cu; /* F2 */
        return true;
    case 0x17u:
        *set1 = 0x3Du; /* F3 */
        return true;
    case 0x1Fu:
        *set1 = 0x3Eu; /* F4 */
        return true;
    case 0x27u:
        *set1 = 0x3Fu; /* F5 */
        return true;
    case 0x2Fu:
        *set1 = 0x40u; /* F6 */
        return true;
    case 0x37u:
        *set1 = 0x41u; /* F7 */
        return true;
    case 0x3Fu:
        *set1 = 0x42u; /* F8 */
        return true;
    case 0x47u:
        *set1 = 0x43u; /* F9 */
        return true;
    case 0x4Fu:
        *set1 = 0x44u; /* F10 */
        return true;
    case 0x56u:
        *set1 = 0x57u; /* F11 */
        return true;
    case 0x5Eu:
        *set1 = 0x58u; /* F12 */
        return true;
    default:
        return false;
    }
}

bool k_input_scancode_translate(k_u8_t set, k_u8_t code, bool extended, k_u8_t *set1_code,
                                bool *set1_extended)
{
    k_u8_t translated = 0;
    bool translated_extended = false;
    bool found;

    if (set1_code == NULL || set1_extended == NULL) {
        return false;
    }

    switch (set) {
    case K_INPUT_SCANCODE_SET2:
        found = translate_set2(code, extended, &translated, &translated_extended);
        break;
    case K_INPUT_SCANCODE_SET3:
        found = translate_set3(code, extended, &translated, &translated_extended);
        break;
    default:
        /* The set 1 is the language itself: every address is already the
         * address the console reads, and the layout decides what it types. */
        translated = code;
        translated_extended = extended;
        found = true;
        break;
    }

    if (!found) {
        return false;
    }

    *set1_code = translated;
    *set1_extended = translated_extended;

    return true;
}

void k_input_decoder_init(struct k_input_decoder *decoder, k_u8_t set)
{
    if (decoder == NULL) {
        return;
    }

    decoder->set = (set == K_INPUT_SCANCODE_SET2 || set == K_INPUT_SCANCODE_SET3)
                       ? set
                       : K_INPUT_SCANCODE_SET1;
    decoder->pause = 0;
    decoder->extended = false;
    decoder->released = false;
}

bool k_input_decoder_feed(struct k_input_decoder *decoder, k_u8_t byte, struct k_input_code *out)
{
    k_u8_t set1_code = 0;
    bool set1_extended = false;
    bool released = false;
    bool extended;

    if (decoder == NULL || out == NULL) {
        return false;
    }

    /* A byte of the sequence of the Pause belongs to that sequence and to no
     * key: the counter drops it and the state of the decoder is untouched. */
    if (decoder->pause != 0u) {
        decoder->pause--;
        return false;
    }

    if (byte == 0xE1u) {
        /* The Pause of a PC: three bytes in the set 1, eight in the set 2, and
         * one address of its own in the set 3 - which is a key of nothing and
         * is dropped either way. */
        if (decoder->set == K_INPUT_SCANCODE_SET1) {
            decoder->pause = 2u;
        } else if (decoder->set == K_INPUT_SCANCODE_SET2) {
            decoder->pause = 7u;
        }

        return false;
    }

    if (byte == 0xE0u) {
        decoder->extended = true;
        return false;
    }

    if (decoder->set != K_INPUT_SCANCODE_SET1 && byte == 0xF0u) {
        decoder->released = true;
        return false;
    }

    extended = decoder->extended;
    decoder->extended = false;

    if (decoder->set == K_INPUT_SCANCODE_SET1) {
        /* The break of the set 1 is the make code with the high bit set, and
         * the whole of the byte is the address: there is no prefix to keep. */
        if ((byte & 0x80u) != 0u) {
            released = true;
            byte = (k_u8_t)(byte & 0x7Fu);
        }
    } else {
        released = decoder->released;
        decoder->released = false;
    }

    if (!k_input_scancode_translate(decoder->set, byte, extended, &set1_code, &set1_extended)) {
        return false;
    }

    out->code = set1_code;
    out->extended = set1_extended;
    out->released = released;

    return true;
}
