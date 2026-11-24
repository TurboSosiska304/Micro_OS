/*
 * Micro OS — the reports of a keyboard of the USB as the keys of the machine.
 *
 * The keyboard of the USB speaks a page of usages and not the scancodes of
 * the PC, and it reports a state and not an edge: these cases hand the
 * portable decoder (kernel/input_hid.c) the eight-byte reports of the boot
 * protocol and read the events back. The truth they hold is the table of the
 * HID specification and the contract of the machine: the same keycap is the
 * same number whichever keyboard is under the hands, a release is the usage
 * that left the six slots, a report that repeats is the typematic echo, and
 * Caps Lock and Ctrl change the character of a key and not its number.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/input.h"
#include "kernel/input_hid.h"

/* One report of eight bytes: the modifiers and the six usages. */
static void report(k_u8_t *bytes, k_u8_t modifiers, const k_u8_t *usages, int count)
{
    int index;

    memset(bytes, 0, K_INPUT_HID_REPORT);
    bytes[0] = modifiers;

    for (index = 0; index < count && index < (int)K_INPUT_HID_KEYS; index++) {
        bytes[2 + index] = usages[index];
    }
}

/* The keys and the characters of one report, as the table of the case. */
struct expected {
    k_u16_t key;
    k_u16_t value;
    char character;
};

static int decode(struct k_input_hid_state *state, const k_u8_t *bytes,
                  struct k_input_hid_event *events)
{
    return (int)k_input_hid_decode(state, bytes, events, 32);
}

static void expect_events(const struct k_input_hid_event *events, int count,
                          const struct expected *table, int expected_count, const char *what)
{
    int index;

    if (count != expected_count) {
        ktest_checks++;
        ktest_fail(__FILE__, __LINE__, "%s: %d event(s), expected %d", what, count, expected_count);
        return;
    }

    for (index = 0; index < count; index++) {
        ktest_checks++;
        if (events[index].key != table[index].key || events[index].value != table[index].value ||
            events[index].character != table[index].character) {
            ktest_fail(__FILE__, __LINE__,
                       "%s: event %d is key %u value %u char %d, expected key %u value %u "
                       "char %d",
                       what, index, (unsigned)events[index].key, (unsigned)events[index].value,
                       (int)events[index].character, (unsigned)table[index].key,
                       (unsigned)table[index].value, (int)table[index].character);
        }
    }
}

/* The table of the usages: a key of the left of the keyboard and a hole. */
static void test_usages(void)
{
    KT_CHECK_INT_EQ(k_input_hid_key(0x04), K_INPUT_KEY_A);
    KT_CHECK_INT_EQ(k_input_hid_key(0x1D), K_INPUT_KEY_Z);
    KT_CHECK_INT_EQ(k_input_hid_key(0x1E), K_INPUT_KEY_1);
    KT_CHECK_INT_EQ(k_input_hid_key(0x26), K_INPUT_KEY_9);
    KT_CHECK_INT_EQ(k_input_hid_key(0x27), K_INPUT_KEY_0);
    KT_CHECK_INT_EQ(k_input_hid_key(0x28), K_INPUT_KEY_ENTER);
    KT_CHECK_INT_EQ(k_input_hid_key(0x29), K_INPUT_KEY_ESC);
    KT_CHECK_INT_EQ(k_input_hid_key(0x2A), K_INPUT_KEY_BACKSPACE);
    KT_CHECK_INT_EQ(k_input_hid_key(0x2B), K_INPUT_KEY_TAB);
    KT_CHECK_INT_EQ(k_input_hid_key(0x2C), K_INPUT_KEY_SPACE);
    KT_CHECK_INT_EQ(k_input_hid_key(0x31), K_INPUT_KEY_BACKSLASH);
    KT_CHECK_INT_EQ(k_input_hid_key(0x39), K_INPUT_KEY_CAPSLOCK);
    KT_CHECK_INT_EQ(k_input_hid_key(0x3A), K_INPUT_KEY_F1);
    KT_CHECK_INT_EQ(k_input_hid_key(0x45), K_INPUT_KEY_F12);
    KT_CHECK_INT_EQ(k_input_hid_key(0x4F), K_INPUT_KEY_RIGHT);
    KT_CHECK_INT_EQ(k_input_hid_key(0x50), K_INPUT_KEY_LEFT);
    KT_CHECK_INT_EQ(k_input_hid_key(0x51), K_INPUT_KEY_DOWN);
    KT_CHECK_INT_EQ(k_input_hid_key(0x52), K_INPUT_KEY_UP);
    KT_CHECK_INT_EQ(k_input_hid_key(0x00), K_INPUT_KEY_NONE);
    KT_CHECK_INT_EQ(k_input_hid_key(0x32), K_INPUT_KEY_NONE); /* the non-US # */
    KT_CHECK_INT_EQ(k_input_hid_key(0x70), K_INPUT_KEY_NONE);
}

/* A press and a release, the two edges of one key. */
static void test_press_and_release(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[8];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t a[] = {0x04};
    int count;

    k_input_hid_init(&state);

    report(bytes, 0, a, 1);
    count = decode(&state, bytes, events);
    {
        const struct expected table[] = {{K_INPUT_KEY_A, INPUT_VALUE_PRESS, 'a'}};

        expect_events(events, count, table, 1, "the press of a");
    }

    report(bytes, 0, NULL, 0);
    count = decode(&state, bytes, events);
    {
        const struct expected table[] = {{K_INPUT_KEY_A, INPUT_VALUE_RELEASE, 0}};

        expect_events(events, count, table, 1, "the release of a");
    }

    /* The next report of nothing is not an event: the state is already
     * empty and a keyboard that is idle says nothing. */
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 0);
}

/* The modifiers are events of their own, and the shift changes the letter. */
static void test_shift(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[8];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t a[] = {0x04};
    int count;

    k_input_hid_init(&state);

    report(bytes, 0x02, a, 1);
    count = decode(&state, bytes, events);
    {
        const struct expected table[] = {{K_INPUT_KEY_SHIFT, INPUT_VALUE_PRESS, 0},
                                         {K_INPUT_KEY_A, INPUT_VALUE_PRESS, 'A'}};

        expect_events(events, count, table, 2, "shift and a in one report");
    }

    /* The shift goes up while the letter stays: one release, and the letter
     * is still held, so there is no second press of it. */
    report(bytes, 0x00, a, 1);
    count = decode(&state, bytes, events);
    {
        const struct expected table[] = {{K_INPUT_KEY_SHIFT, INPUT_VALUE_RELEASE, 0}};

        expect_events(events, count, table, 1, "the release of the shift");
    }

    /* The right shift is the same key of the machine, and the letter left
     * the report at the same time: two events, the shift down and the
     * release of the key. */
    report(bytes, 0x20, NULL, 0);
    count = decode(&state, bytes, events);
    {
        const struct expected table[] = {{K_INPUT_KEY_SHIFT, INPUT_VALUE_PRESS, 0},
                                         {K_INPUT_KEY_A, INPUT_VALUE_RELEASE, 0}};

        expect_events(events, count, table, 2, "the right shift");
    }
}

/* The shifted row of the digits and the punctuation. */
static void test_shifted_characters(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[8];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t one[] = {0x1E};
    k_u8_t slash[] = {0x38};
    k_u8_t minus[] = {0x2D};
    int count;

    k_input_hid_init(&state);
    report(bytes, 0x02, one, 1);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 2);
    KT_CHECK_INT_EQ(events[1].character, '!');

    report(bytes, 0x00, NULL, 0);
    (void)decode(&state, bytes, events);

    report(bytes, 0x02, slash, 1);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 2);
    KT_CHECK_INT_EQ(events[1].character, '?');

    report(bytes, 0x00, NULL, 0);
    (void)decode(&state, bytes, events);

    report(bytes, 0x02, minus, 1);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 2);
    KT_CHECK_INT_EQ(events[1].character, '_');
}

/* Caps Lock is a lock of the letters and nothing else, as on the PS/2. */
static void test_caps_lock(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[8];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t caps[] = {0x39};
    k_u8_t a[] = {0x04};
    k_u8_t one[] = {0x1E};
    int count;

    k_input_hid_init(&state);

    report(bytes, 0, caps, 1);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_CAPSLOCK);
    KT_CHECK_INT_EQ(events[0].value, INPUT_VALUE_PRESS);
    KT_CHECK_INT_EQ(events[0].character, 0);
    KT_CHECK(state.caps);

    report(bytes, 0, NULL, 0);
    (void)decode(&state, bytes, events);

    report(bytes, 0, a, 1);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(events[0].character, 'A');

    report(bytes, 0, NULL, 0);
    (void)decode(&state, bytes, events);

    /* A digit is not a letter: the lock does not touch it. */
    report(bytes, 0, one, 1);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(events[0].character, '1');

    report(bytes, 0, NULL, 0);
    (void)decode(&state, bytes, events);

    /* The second press turns it off. */
    report(bytes, 0, caps, 1);
    (void)decode(&state, bytes, events);
    KT_CHECK(!state.caps);

    report(bytes, 0, NULL, 0);
    (void)decode(&state, bytes, events);

    report(bytes, 0, a, 1);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(events[0].character, 'a');
}

/* Ctrl with a letter is the control code of that letter. */
static void test_ctrl(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[8];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t c[] = {0x06};
    int count;

    k_input_hid_init(&state);

    report(bytes, 0x01, c, 1);
    count = decode(&state, bytes, events);
    {
        const struct expected table[] = {{K_INPUT_KEY_CTRL, INPUT_VALUE_PRESS, 0},
                                         {K_INPUT_KEY_C, INPUT_VALUE_PRESS, 3}};

        expect_events(events, count, table, 2, "Ctrl+C");
    }

    /* The right Ctrl is the same key of the machine: the left one goes up,
     * the right one comes down, and the `c` of the report before is no
     * longer held. */
    report(bytes, 0x10, NULL, 0);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 3);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_CTRL);
    KT_CHECK_INT_EQ(events[0].value, INPUT_VALUE_RELEASE);
    KT_CHECK_INT_EQ(events[1].key, K_INPUT_KEY_CTRL);
    KT_CHECK_INT_EQ(events[1].value, INPUT_VALUE_PRESS);
    KT_CHECK_INT_EQ(events[2].key, K_INPUT_KEY_C);
    KT_CHECK_INT_EQ(events[2].value, INPUT_VALUE_RELEASE);
}

/* Six keys down at once, and the difference of the next report. */
static void test_rollover(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[16];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t six[] = {0x04, 0x05, 0x06, 0x07, 0x08, 0x09};
    k_u8_t five[] = {0x04, 0x05, 0x06, 0x07, 0x08};
    int count;
    int index;

    k_input_hid_init(&state);

    report(bytes, 0, six, 6);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 6);
    for (index = 0; index < 6; index++) {
        KT_CHECK_INT_EQ(events[index].key, (k_u16_t)(K_INPUT_KEY_A + index));
        KT_CHECK_INT_EQ(events[index].value, INPUT_VALUE_PRESS);
        KT_CHECK_INT_EQ(events[index].character, 'a' + index);
    }

    /* The key that left the six is the key that was released, whatever its
     * slot: the report of the device is a set and not a sequence. */
    report(bytes, 0, five, 5);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_F);
    KT_CHECK_INT_EQ(events[0].value, INPUT_VALUE_RELEASE);

    /* A usage that is twice in the report is one key. */
    {
        struct k_input_hid_state fresh;
        k_u8_t twice[] = {0x04, 0x04, 0x00};

        k_input_hid_init(&fresh);

        report(bytes, 0, twice, 3);
        count = (int)k_input_hid_decode(&fresh, bytes, events, 32);
        KT_CHECK_INT_EQ(count, 1);
        KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_A);
        KT_CHECK_INT_EQ(events[0].value, INPUT_VALUE_PRESS);
    }
}

/* A repeated report is the typematic echo: characters and no edge. */
static void test_repeat(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[8];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t a[] = {0x04};
    int count;

    k_input_hid_init(&state);

    report(bytes, 0, a, 1);
    (void)decode(&state, bytes, events);

    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_A);
    KT_CHECK_INT_EQ(events[0].value, INPUT_VALUE_PRESS);
    KT_CHECK_INT_EQ(events[0].character, 'a');
    KT_CHECK(events[0].repeat);

    /* Shift and a held letter: the repeat is the shifted character. */
    report(bytes, 0x02, a, 1);
    (void)decode(&state, bytes, events);

    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(events[0].character, 'A');
    KT_CHECK(events[0].repeat);
}

/* The keys a line needs: Enter, Backspace and Tab, and the block of the
 * cursor, which has a number of the machine and no character. */
static void test_line_keys(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[8];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t enter[] = {0x28};
    int count;

    k_input_hid_init(&state);

    report(bytes, 0, enter, 1);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_ENTER);
    KT_CHECK_INT_EQ(events[0].character, '\r');

    report(bytes, 0, NULL, 0);
    (void)decode(&state, bytes, events);

    {
        k_u8_t tab[] = {0x2B};
        k_u8_t backspace[] = {0x2A};
        k_u8_t up[] = {0x52};

        report(bytes, 0, tab, 1);
        count = decode(&state, bytes, events);
        KT_CHECK_INT_EQ(count, 1);
        KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_TAB);
        KT_CHECK_INT_EQ(events[0].character, 0);

        report(bytes, 0, NULL, 0);
        (void)decode(&state, bytes, events);

        report(bytes, 0, backspace, 1);
        count = decode(&state, bytes, events);
        KT_CHECK_INT_EQ(count, 1);
        KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_BACKSPACE);
        KT_CHECK_INT_EQ(events[0].character, '\b');

        report(bytes, 0, NULL, 0);
        (void)decode(&state, bytes, events);

        report(bytes, 0, up, 1);
        count = decode(&state, bytes, events);
        KT_CHECK_INT_EQ(count, 1);
        KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_UP);
        KT_CHECK_INT_EQ(events[0].character, 0);
    }
}

/* An unknown usage is not a key and does not disturb the keys beside it. */
static void test_unknown_usage(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[8];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t keys[] = {0x04, 0x70, 0x05};
    int count;

    k_input_hid_init(&state);

    report(bytes, 0, keys, 3);
    count = decode(&state, bytes, events);
    KT_CHECK_INT_EQ(count, 2);
    KT_CHECK_INT_EQ(events[0].key, K_INPUT_KEY_A);
    KT_CHECK_INT_EQ(events[1].key, K_INPUT_KEY_B);
}

/* The cap of the answer: a report of more events than the caller has room
 * for is cut, and the state still follows the report. */
static void test_cap(void)
{
    struct k_input_hid_state state;
    struct k_input_hid_event events[2];
    k_u8_t bytes[K_INPUT_HID_REPORT];
    k_u8_t keys[] = {0x04, 0x05, 0x06};
    int count;

    k_input_hid_init(&state);

    report(bytes, 0, keys, 3);
    count = (int)k_input_hid_decode(&state, bytes, events, 2);
    KT_CHECK_INT_EQ(count, 2);
    KT_CHECK_INT_EQ(state.keys[0], 0x04);
    KT_CHECK_INT_EQ(state.keys[2], 0x06);
    KT_CHECK(state.seen);
}

void test_input_hid(void)
{
    test_usages();
    test_press_and_release();
    test_shift();
    test_shifted_characters();
    test_caps_lock();
    test_ctrl();
    test_rollover();
    test_repeat();
    test_line_keys();
    test_unknown_usage();
    test_cap();
}
