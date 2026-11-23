/*
 * Micro OS — one keyboard, three scancode sets, one set of keys.
 *
 * The keyboard of a laptop of UEFI speaks the set 2, the keyboard of QEMU the
 * set 2 of its own emulation behind the translation of its 8042 (which is the
 * set 1 at the port), and a machine of the set 3 is a workstation of the
 * 1990s - and the console of the kernel is one program that types one letter
 * whichever of them a person has under the hands.
 *
 * The translation and the decoder are pure (kernel/input_scancode.c): they
 * turn a byte into the address of the set 1 and a key of the machine, and
 * this test drives them byte by byte, without a keyboard. What it holds is
 * the truth of the three tables of the specification: the same keycap is the
 * same key in the three sets, the extended block of the set 1 and of the set 2
 * is the same keys, the break of a key is a release in every set, and rubbish
 * on the line does not cost the key that comes after it.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/input.h"
#include "kernel/input_scancode.h"
#include "kernel/input_set1.h"

/*
 * One key of the keyboard in the three sets: the address of it in each (the
 * make code, without the prefix), whether the set 1 carries it behind the
 * prefix 0xE0, and the name the machine gives it.
 *
 * The addresses are the table of the three sets of the PC, key by key. The
 * interesting ones are the keys whose address moved between the sets, and
 * they are all here: the Esc of the set 3 (0x08 - the address of the
 * Backspace of the set 1), the Caps Lock and the Ctrl of the left (which
 * swapped), the F7 and the F9 of the set 2 (0x83 and 0x01, the last of them
 * the Esc of the set 1), the operators of the keypad, and the whole block of
 * the cursor, which the set 3 gives an address of a byte.
 */
struct key_of_sets {
    const char *name;
    k_u8_t set1;
    k_u8_t set2;
    k_u8_t set3;
    bool extended; /* the address of the set 1 is the second form (0xE0) */
    k_u16_t key;   /* the name of the machine */
};

static const struct key_of_sets g_keys[] = {
    {"a", 0x1E, 0x1C, 0x1C, false, K_INPUT_KEY_A},
    {"b", 0x30, 0x32, 0x32, false, K_INPUT_KEY_B},
    {"q", 0x10, 0x15, 0x15, false, K_INPUT_KEY_Q},
    {"w", 0x11, 0x1D, 0x1D, false, K_INPUT_KEY_W},
    {"z", 0x2C, 0x1A, 0x1A, false, K_INPUT_KEY_Z},
    {"1", 0x02, 0x16, 0x16, false, K_INPUT_KEY_1},
    {"0", 0x0B, 0x45, 0x45, false, K_INPUT_KEY_0},
    {"-", 0x0C, 0x4E, 0x4E, false, K_INPUT_KEY_MINUS},
    {"=", 0x0D, 0x55, 0x55, false, K_INPUT_KEY_EQUAL},
    {"[", 0x1A, 0x54, 0x54, false, K_INPUT_KEY_LEFTBRACE},
    {"]", 0x1B, 0x5B, 0x5B, false, K_INPUT_KEY_RIGHTBRACE},
    {"\\", 0x2B, 0x5D, 0x5C, false, K_INPUT_KEY_BACKSLASH},
    {";", 0x27, 0x4C, 0x4C, false, K_INPUT_KEY_SEMICOLON},
    {"'", 0x28, 0x52, 0x52, false, K_INPUT_KEY_APOSTROPHE},
    {"`", 0x29, 0x0E, 0x0E, false, K_INPUT_KEY_GRAVE},
    {",", 0x33, 0x41, 0x41, false, K_INPUT_KEY_COMMA},
    {".", 0x34, 0x49, 0x49, false, K_INPUT_KEY_DOT},
    {"/", 0x35, 0x4A, 0x4A, false, K_INPUT_KEY_SLASH},
    /* The keys a line needs. The Esc of the set 3 is the address of the
     * Backspace of the set 1. */
    {"esc", 0x01, 0x76, 0x08, false, K_INPUT_KEY_ESC},
    {"tab", 0x0F, 0x0D, 0x0D, false, K_INPUT_KEY_TAB},
    {"backspace", 0x0E, 0x66, 0x66, false, K_INPUT_KEY_BACKSPACE},
    {"enter", 0x1C, 0x5A, 0x5A, false, K_INPUT_KEY_ENTER},
    {"space", 0x39, 0x29, 0x29, false, K_INPUT_KEY_SPACE},
    /* The modifiers: the Caps Lock and the Ctrl of the left swapped in the
     * set 3, and the Alt of the left moved to the address of the Caps Lock
     * of the set 2. */
    {"lshift", 0x2A, 0x12, 0x12, false, K_INPUT_KEY_SHIFT},
    {"rshift", 0x36, 0x59, 0x59, false, K_INPUT_KEY_SHIFT},
    {"lctrl", 0x1D, 0x14, 0x11, false, K_INPUT_KEY_CTRL},
    {"lalt", 0x38, 0x11, 0x19, false, K_INPUT_KEY_ALT},
    {"caps", 0x3A, 0x58, 0x14, false, K_INPUT_KEY_CAPSLOCK},
    /* The function keys of the set 2 and of the set 3, whose addresses are
     * not the ones of the set 1 at all. */
    {"f1", 0x3B, 0x05, 0x07, false, K_INPUT_KEY_F1},
    {"f7", 0x41, 0x83, 0x37, false, K_INPUT_KEY_F7},
    {"f9", 0x43, 0x01, 0x47, false, K_INPUT_KEY_F9},
    {"f12", 0x58, 0x07, 0x5E, false, K_INPUT_KEY_F12},
    /* The keypad: the digits the console types, and the two keys the set 1
     * carries behind the prefix. */
    {"kp7", 0x47, 0x6C, 0x6C, false, K_INPUT_KEY_NONE},
    {"kp0", 0x52, 0x70, 0x70, false, K_INPUT_KEY_NONE},
    {"kp*", 0x37, 0x7C, 0x7E, false, K_INPUT_KEY_NONE},
    {"kp-", 0x4A, 0x7B, 0x84, false, K_INPUT_KEY_NONE},
    {"kp+", 0x4E, 0x79, 0x7C, false, K_INPUT_KEY_NONE},
    {"kpenter", 0x1C, 0x5A, 0x79, true, K_INPUT_KEY_ENTER},
    {"kpslash", 0x35, 0x4A, 0x77, true, K_INPUT_KEY_SLASH},
    /* The cursor block of the set 1 and of the set 2 (behind 0xE0) and of the
     * set 3 (an address of its own). */
    {"up", 0x48, 0x75, 0x63, true, K_INPUT_KEY_UP},
    {"down", 0x50, 0x72, 0x60, true, K_INPUT_KEY_DOWN},
    {"left", 0x4B, 0x6B, 0x61, true, K_INPUT_KEY_LEFT},
    {"right", 0x4D, 0x74, 0x6A, true, K_INPUT_KEY_RIGHT},
    {"home", 0x47, 0x6C, 0x6E, true, K_INPUT_KEY_HOME},
    {"end", 0x4F, 0x69, 0x65, true, K_INPUT_KEY_END},
    {"pageup", 0x49, 0x7D, 0x6F, true, K_INPUT_KEY_PAGEUP},
    {"pagedown", 0x51, 0x7A, 0x6D, true, K_INPUT_KEY_PAGEDOWN},
    {"insert", 0x52, 0x70, 0x67, true, K_INPUT_KEY_INSERT},
    {"delete", 0x53, 0x71, 0x64, true, K_INPUT_KEY_DELETE},
    /* The two modifiers of the right and the Print Screen. */
    {"rctrl", 0x1D, 0x14, 0x58, true, K_INPUT_KEY_CTRL},
    {"ralt", 0x38, 0x11, 0x39, true, K_INPUT_KEY_ALT},
    {"print", 0x37, 0x7C, 0x57, true, K_INPUT_KEY_NONE},
};

/* The keys of one stream of bytes, as one decoder read them. */
struct stream {
    k_u32_t count;
    struct k_input_code codes[8];
};

static void feed(struct k_input_decoder *decoder, const k_u8_t *bytes, k_size_t count,
                 struct stream *out)
{
    k_size_t i;

    memset(out, 0, sizeof(*out));

    for (i = 0; i < count; i++) {
        struct k_input_code code;

        if (!k_input_decoder_feed(decoder, bytes[i], &code)) {
            continue;
        }

        if (out->count < K_ARRAY_SIZE(out->codes)) {
            out->codes[out->count] = code;
        }

        out->count++;
    }
}

/* The same keycap is the same key of the machine in the three sets, and the
 * same address of the set 1. */
static void check_translation(void)
{
    k_u8_t code = 0;
    bool extended = false;
    k_size_t i;

    ktest_begin("ps2: one keycap is one key in the set 1, the set 2 and the set 3");

    for (i = 0; i < K_ARRAY_SIZE(g_keys); i++) {
        const struct key_of_sets *key = &g_keys[i];

        /* The set 1 is the language itself: its address answers itself. */
        KT_CHECK(k_input_scancode_translate(K_INPUT_SCANCODE_SET1, key->set1, key->extended, &code,
                                            &extended));
        KT_CHECK_INT_EQ((int)code, key->set1);
        KT_CHECK_INT_EQ((int)extended, key->extended);

        /* The set 2 carries the same keys behind the same prefix, and the
         * set 3 gives them an address of their own. */
        KT_CHECK(k_input_scancode_translate(K_INPUT_SCANCODE_SET2, key->set2, key->extended, &code,
                                            &extended));
        KT_CHECK_INT_EQ((int)code, key->set1);
        KT_CHECK_INT_EQ((int)extended, key->extended);

        KT_CHECK(
            k_input_scancode_translate(K_INPUT_SCANCODE_SET3, key->set3, false, &code, &extended));
        KT_CHECK_INT_EQ((int)code, key->set1);
        KT_CHECK_INT_EQ((int)extended, key->extended);

        /* And the name of the machine: the two tables meet here, the one of
         * the sets and the one of the set 1. */
        KT_CHECK_INT_EQ(k_input_set1_key(key->set1, key->extended), key->key);
    }

    /* A prefix of the set 2 is not an address, and the keys no console of
     * this system has are not keys: the Windows keys, the media block, the
     * Pause of the set 3, and a byte that is nothing at all. */
    KT_CHECK(!k_input_scancode_translate(K_INPUT_SCANCODE_SET2, 0x1Cu, false, NULL, NULL));
    KT_CHECK(!k_input_scancode_translate(K_INPUT_SCANCODE_SET2, 0x1Fu, true, &code, &extended));
    KT_CHECK(!k_input_scancode_translate(K_INPUT_SCANCODE_SET2, 0x3Fu, true, &code, &extended));
    KT_CHECK(!k_input_scancode_translate(K_INPUT_SCANCODE_SET2, 0xE0u, false, &code, &extended));
    KT_CHECK(!k_input_scancode_translate(K_INPUT_SCANCODE_SET2, 0xF0u, false, &code, &extended));
    KT_CHECK(!k_input_scancode_translate(K_INPUT_SCANCODE_SET3, 0x62u, false, &code, &extended));
    KT_CHECK(!k_input_scancode_translate(K_INPUT_SCANCODE_SET3, 0x75u, true, &code, &extended));
    KT_CHECK(!k_input_scancode_translate(K_INPUT_SCANCODE_SET2, 0xFAu, false, &code, &extended));
}

/* A make and a break of one key, in the three sets, through the decoder: the
 * same key of the machine, the same address, and the same edge. */
static void check_edges(void)
{
    static const k_u8_t a_set1[] = {0x1E, 0x9E};
    static const k_u8_t a_set2[] = {0x1C, 0xF0, 0x1C};
    static const k_u8_t a_set3[] = {0x1C, 0xF0, 0x1C};
    static const struct {
        k_u8_t set;
        const k_u8_t *bytes;
        k_size_t count;
    } streams[] = {
        {K_INPUT_SCANCODE_SET1, a_set1, sizeof(a_set1)},
        {K_INPUT_SCANCODE_SET2, a_set2, sizeof(a_set2)},
        {K_INPUT_SCANCODE_SET3, a_set3, sizeof(a_set3)},
    };
    k_size_t i;

    ktest_begin("ps2: the make and the break of a key are one press and one release");

    for (i = 0; i < K_ARRAY_SIZE(streams); i++) {
        struct k_input_decoder decoder;
        struct stream stream;

        k_input_decoder_init(&decoder, streams[i].set);
        feed(&decoder, streams[i].bytes, streams[i].count, &stream);

        KT_CHECK_INT_EQ((int)stream.count, 2);

        if (stream.count != 2) {
            continue;
        }

        KT_CHECK_INT_EQ((int)stream.codes[0].code, 0x1E);
        KT_CHECK(!stream.codes[0].extended);
        KT_CHECK(!stream.codes[0].released);

        KT_CHECK_INT_EQ((int)stream.codes[1].code, 0x1E);
        KT_CHECK(stream.codes[1].released);
    }
}

/* The extended block of the set 1 and of the set 2: the arrows, the block
 * above them and the two modifiers of the right, which the set 1 carries
 * behind 0xE0 and the set 2 behind 0xE0 as well - and the set 3 with an
 * address of its own. */
static void check_extended(void)
{
    static const k_u8_t up_set1[] = {0xE0, 0x48, 0xE0, 0xC8};
    static const k_u8_t up_set2[] = {0xE0, 0x75, 0xE0, 0xF0, 0x75};
    static const k_u8_t up_set3[] = {0x63, 0xF0, 0x63};
    static const k_u8_t delete_set2[] = {0xE0, 0x71};
    static const k_u8_t rctrl_set2[] = {0xE0, 0x14};
    static const k_u8_t kpenter_set2[] = {0xE0, 0x5A};
    struct k_input_decoder decoder;
    struct stream stream;

    ktest_begin("ps2: the extended block is the same keys in the set 1 and the set 2");

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET1);
    feed(&decoder, up_set1, sizeof(up_set1), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 2);
    KT_CHECK_INT_EQ((int)stream.codes[0].code, 0x48);
    KT_CHECK(stream.codes[0].extended);
    KT_CHECK(!stream.codes[0].released);
    KT_CHECK(stream.codes[1].released);

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET2);
    feed(&decoder, up_set2, sizeof(up_set2), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 2);
    KT_CHECK_INT_EQ((int)stream.codes[0].code, 0x48);
    KT_CHECK(stream.codes[0].extended);
    KT_CHECK(!stream.codes[0].released);
    KT_CHECK(stream.codes[1].released);
    KT_CHECK_INT_EQ(k_input_set1_key(stream.codes[0].code, stream.codes[0].extended),
                    K_INPUT_KEY_UP);

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET3);
    feed(&decoder, up_set3, sizeof(up_set3), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 2);
    KT_CHECK_INT_EQ((int)stream.codes[0].code, 0x48);
    KT_CHECK(stream.codes[0].extended);

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET2);
    feed(&decoder, delete_set2, sizeof(delete_set2), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK_INT_EQ(k_input_set1_key(stream.codes[0].code, stream.codes[0].extended),
                    K_INPUT_KEY_DELETE);

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET2);
    feed(&decoder, rctrl_set2, sizeof(rctrl_set2), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK_INT_EQ(k_input_set1_key(stream.codes[0].code, stream.codes[0].extended),
                    K_INPUT_KEY_CTRL);

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET2);
    feed(&decoder, kpenter_set2, sizeof(kpenter_set2), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK_INT_EQ(k_input_set1_key(stream.codes[0].code, stream.codes[0].extended),
                    K_INPUT_KEY_ENTER);
}

/*
 * The rubbish of the line: a byte that is no key, two prefixes in a row, the
 * sequence of the Pause (three bytes in the set 1, eight in the set 2, and no
 * key of the console in either), and the answer of a keyboard that is not a
 * key of a person at the machine. None of them costs the key that comes after
 * it: the state of the decoder is the one it had.
 */
static void check_noise(void)
{
    static const k_u8_t pause_set1[] = {0xE1, 0x1D, 0x45, 0x1E};
    static const k_u8_t pause_set2[] = {0xE1, 0x14, 0x77, 0xE1, 0xF0, 0x14, 0xF0, 0x77, 0x1C};
    static const k_u8_t doubled[] = {0xE0, 0xE0, 0x75};
    static const k_u8_t rubbish_set2[] = {0xFA, 0xEE, 0x00, 0x1C};
    static const k_u8_t lone_break[] = {0xF0, 0xF0, 0x1C};
    struct k_input_decoder decoder;
    struct stream stream;

    ktest_begin("ps2: a byte that is no key does not cost the key that follows it");

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET1);
    feed(&decoder, pause_set1, sizeof(pause_set1), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK_INT_EQ((int)stream.codes[0].code, 0x1E);

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET2);
    feed(&decoder, pause_set2, sizeof(pause_set2), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK_INT_EQ((int)stream.codes[0].code, 0x1E);
    KT_CHECK(!stream.codes[0].released);

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET2);
    feed(&decoder, doubled, sizeof(doubled), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK_INT_EQ(k_input_set1_key(stream.codes[0].code, stream.codes[0].extended),
                    K_INPUT_KEY_UP);

    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET2);
    feed(&decoder, rubbish_set2, sizeof(rubbish_set2), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK_INT_EQ((int)stream.codes[0].code, 0x1E);

    /* Two prefixes of a break and one key: the second prefix is the one that
     * counts, and the key is a release. */
    k_input_decoder_init(&decoder, K_INPUT_SCANCODE_SET3);
    feed(&decoder, lone_break, sizeof(lone_break), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK(stream.codes[0].released);
}

/* A set the machine does not have is the set 1: the decoder of a keyboard that
 * did not answer reads the bytes of every machine of the port. */
static void check_default(void)
{
    static const k_u8_t a_set1[] = {0x1E};
    struct k_input_decoder decoder;
    struct stream stream;

    ktest_begin("ps2: a decoder of no set reads the set 1");

    k_input_decoder_init(&decoder, 0u);
    KT_CHECK_INT_EQ((int)decoder.set, K_INPUT_SCANCODE_SET1);
    feed(&decoder, a_set1, sizeof(a_set1), &stream);
    KT_CHECK_INT_EQ((int)stream.count, 1);
    KT_CHECK_INT_EQ((int)stream.codes[0].code, 0x1E);

    k_input_decoder_init(&decoder, 9u);
    KT_CHECK_INT_EQ((int)decoder.set, K_INPUT_SCANCODE_SET1);
}

void test_ps2(void)
{
    check_translation();
    check_edges();
    check_extended();
    check_noise();
    check_default();
}
