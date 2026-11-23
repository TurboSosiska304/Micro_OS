/*
 * Micro OS — the PS/2 keyboard of the x86-64 port.
 *
 * The keyboard of the PC hangs on the first line of the interrupt controller -
 * the 8259 of the legacy chip, or the I/O APIC that routes the line to any
 * core (ADR 0012) - and speaks the scancode set 1: the 8042 of the machine
 * translates the set 2 of the device
 * into the bytes a keyboard of the 1980s made, and those are the bytes the
 * monitor of QEMU sends too. The handler runs with the interrupts off and
 * does the least it can: read one byte of the data port, translate it, put
 * the character in a ring, acknowledge the chip. A task takes the characters
 * out through k_port_console_poll() and k_port_console_getchar().
 *
 * The map below is the US layout, which is the one the scancodes of the
 * machine carry: letters, digits, the punctuation of a command line and the
 * keys a line needs (Enter, Backspace and Ctrl+C). The E0 sequences arrived
 * with the interface of 1.4.0: the arrows, Home, End, Insert and Delete have
 * a name and an event now (`kernel/input_set1.h`), the right Ctrl is the
 * modifier of the same form and the Enter and the slash of the keypad still
 * type their character for the shell. The function keys type nothing and
 * there is no repeat of a held key of its own because the device repeats it.
 *
 * Since ADR 0028 every key of the set has a name as well and the handler
 * pushes an event beside the character (`/dev/input0`): the Tab, the function
 * keys and the releases have no character, and an interface of a program
 * needs exactly the keys the console cannot carry.
 */
#include "port_x86_64_console.h"

#include "kernel/input.h"
#include "kernel/input_set1.h"
#include "kernel/port/lock.h"
#include "kernel/port/time.h"
#include "port_x86_64.h"
#include "port_x86_64_input.h"

/* The controller of the keyboard, the 8042: a data port and a status port. */
#define PS2_DATA 0x60u
#define PS2_STATUS 0x64u
#define PS2_STATUS_OUTPUT 0x01u /* a byte waits in the data port */
#define PS2_STATUS_INPUT 0x02u  /* the controller is still taking a command */

#define PS2_CMD_READ_CONFIG 0x20u
#define PS2_CMD_WRITE_CONFIG 0x60u
#define PS2_CMD_ENABLE_FIRST 0xAEu
#define PS2_CONFIG_IRQ1 0x01u   /* the first port raises IRQ1 */
#define PS2_CONFIG_CLOCK1 0x10u /* set: the clock of the first port is off */

/* A command of the controller answers in microseconds; a counter is enough
 * of a limit for the one time the boot waits for it, and a machine where it
 * runs out is a machine without a keyboard, not a reason to hang. */
#define PS2_TIMEOUT 100000u

/*
 * The ring of the characters. Sixty-four entries and a power of two: the
 * indices wrap by themselves, and a person typing while the shell prints a
 * screenful of text is the worst case anybody has met.
 */
#define PS2_RING 64u

static volatile k_u8_t g_keys[PS2_RING];
static volatile k_u32_t g_key_head; /* written by IRQ1, read by the tasks */
static volatile k_u32_t g_key_tail; /* written and read by the tasks */

/*
 * The same keys as events (ADR 0028): a moment, a name and the edge. The ring
 * is beside the characters and not instead of them, because the console of the
 * kernel and an interface of a program are two readers of one keyboard; the
 * capture below says which of them is listening.
 */
#define PS2_EVENT_RING 64u

static volatile struct input_event g_events[PS2_EVENT_RING];
static volatile k_u32_t g_event_head; /* written by IRQ1 */
static volatile k_u32_t g_event_tail; /* written and read by the tasks */

/* While a program holds the keyboard, the keys are events and not characters:
 * a `q` of an interface must not become a line of the shell that waits for
 * the interface to end. */
static bool g_capture;

/* The state of the modifiers, as the make and the break codes left it. */
static bool g_shift_left;
static bool g_shift_right;
static bool g_ctrl;
static bool g_caps;

/* The byte behind 0xE0 belongs to the second form of the set: the arrows,
 * Home, End, Insert, Delete, the keypad Enter, the right Ctrl. The prefix is
 * one byte of state, and the key it carries goes down and up like any other
 * (the break code is 0xE0 followed by the address with the top bit set). */
static bool g_extended;

/* 0xE1 (the Pause of a PC) is a sequence of six bytes and none of them is a
 * key of this console: the byte after it is dropped, as the whole sequence
 * was before the arrows arrived. */
static bool g_skip_next;

/*
 * The characters of the set 1 by scancode, unshifted, one row of sixteen
 * scancodes to a line: the row is the high nibble of the address. A zero is
 * "this key is not a character" - a modifier the handler takes for itself, a
 * key that is not on this console, or a slot the map does not use.
 *
 * The rows are the keys of the address in the order of the set: Esc, 1..0,
 * -, = and Backspace in the first; the letters of the top, the middle and
 * the bottom row in the next three; the shifts, the space, Caps Lock and the
 * first function keys after them; the pad and the rest at the end.
 *
 * Tab is left out although the screen understands it: the shell of the
 * kernel splits a line on spaces, and a tab in the middle of a command would
 * arrive as part of a word instead of as a separator.
 */
#define PS2_ROWS 6u

static const char g_normal[PS2_ROWS][16] = {
    {0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', 0},
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\r', 0, 'a', 's'},
    {'d', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v'},
    {'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1'},
    {'2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

/* The same map with the shift held: zero means "the unshifted entry is the
 * one to use". Caps Lock is not here at all: on a PC it changes the letters
 * and nothing else. */
static const char g_shifted[PS2_ROWS][16] = {
    {0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '{', '}', 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, ':', '"', '~', 0, '|', 0, 0, 0, 0},
    {0, 0, 0, '<', '>', '?', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static void key_push(k_u8_t character)
{
    k_u32_t next = (g_key_head + 1u) % PS2_RING;

    if (next == g_key_tail) {
        /* A full ring loses the new key and keeps the order of the old ones:
         * a person typing faster than the shell reads is asking for a queue,
         * and the queue it got is the one it has. */
        return;
    }

    g_keys[g_key_head] = character;
    g_key_head = next;
}

/* One event into the ring of the interface. A full ring loses the newest
 * event, the same rule as the characters: the order of the old ones is what
 * a focus follows. */
static void event_push(k_u16_t code, k_u16_t value)
{
    k_u32_t next = (g_event_head + 1u) % PS2_EVENT_RING;

    if (next == g_event_tail) {
        return;
    }

    g_events[g_event_head].ms = (k_u32_t)k_port_uptime_ms();
    g_events[g_event_head].code = code;
    g_events[g_event_head].value = value;
    g_event_head = next;
}

bool port_x86_64_input_event(struct input_event *out)
{
    k_u32_t tail = g_event_tail;

    if (out == NULL || tail == g_event_head) {
        return false;
    }

    out->ms = g_events[tail].ms;
    out->code = g_events[tail].code;
    out->value = g_events[tail].value;
    g_event_tail = (tail + 1u) % PS2_EVENT_RING;

    return true;
}

void port_x86_64_input_capture(bool on)
{
    /*
     * The rings are emptied when the owner changes: a key pressed at the
     * prompt before the interface started is not a key of the interface, and
     * a `q` of the interface is not a command of the shell. The section has
     * the interrupts of the core off, so IRQ1 does not push between the two
     * lines.
     */
    k_port_enter_critical();
    g_capture = on;
    g_key_tail = g_key_head;
    g_event_tail = g_event_head;
    k_port_exit_critical();
}

int port_x86_64_ps2_poll(void)
{
    k_u32_t tail = g_key_tail;
    int character;

    if (tail == g_key_head) {
        return -1;
    }

    character = (int)g_keys[tail];
    g_key_tail = (tail + 1u) % PS2_RING;

    return character;
}

static char key_translate(k_u8_t scancode)
{
    k_u8_t row = (k_u8_t)(scancode >> 4);
    k_u8_t column = (k_u8_t)(scancode & 0x0Fu);
    char plain;
    bool shifted;

    if (row >= PS2_ROWS) {
        return 0;
    }

    plain = g_normal[row][column];
    if (plain == 0) {
        return 0;
    }

    if (g_ctrl) {
        /* Ctrl with a letter is the control code of that letter: Ctrl+C is
         * the one the shell of the kernel reads as "throw the line away".
         * The other combinations of the PC are not the console's. */
        if (plain >= 'a' && plain <= 'z') {
            return (char)(plain - 'a' + 1);
        }
        return 0;
    }

    shifted = g_shift_left || g_shift_right;

    if (plain >= 'a' && plain <= 'z') {
        /* A letter is capital when the shift is held xor the lock is on:
         * that is the whole of what Caps Lock does on a PC. */
        if (shifted != g_caps) {
            return (char)(plain - 'a' + 'A');
        }
        return plain;
    }

    if (shifted && g_shifted[row][column] != 0) {
        return g_shifted[row][column];
    }

    return plain;
}

/*
 * One byte of the device. The break codes are the make codes with the bit 7
 * set, so a key that goes up is the same number: the modifiers are tracked
 * on both edges, and every other key only acts when it goes down.
 *
 * Every byte is also an event of `/dev/input0` (ADR 0028), both edges
 * included: an interface that draws a key being held needs the release, which
 * the character stream of the console has no way to say.
 *
 * The E0 prefix is a key of the second form and not a byte to drop (1.4.0):
 * the arrows, Home, End, Insert and Delete have no address of the first 128,
 * and a program of a text already asks for them. The right Ctrl is the
 * modifier of the same form, and the Enter and the slash of the keypad still
 * type their character for the shell.
 */
static void key_handle(k_u8_t code)
{
    bool released;
    bool extended;
    k_u8_t scancode;
    k_u16_t key;
    char character;

    if (code == 0xE0u) {
        g_extended = true;
        return;
    }

    if (code == 0xE1u) {
        g_skip_next = true;
        return;
    }

    if (g_skip_next) {
        g_skip_next = false;
        return;
    }

    extended = g_extended;
    g_extended = false;
    released = (code & 0x80u) != 0;
    scancode = code & 0x7Fu;
    key = k_input_set1_key(scancode, extended);

    if (key != K_INPUT_KEY_NONE) {
        event_push(key, released ? INPUT_VALUE_RELEASE : INPUT_VALUE_PRESS);
    }

    if (extended) {
        /* The right Ctrl is a modifier of the second form; the right Alt and
         * the rest of the block are events and no characters of the console. */
        if (scancode == 0x1Du) {
            g_ctrl = !released;
            return;
        }

        if (released || g_capture) {
            return;
        }

        /* The Enter and the slash of the keypad are the keys the shell types. */
        if (scancode == 0x1Cu) {
            key_push('\r');
        } else if (scancode == 0x35u) {
            key_push('/');
        }
        return;
    }

    switch (scancode) {
    case 0x2Au: /* left shift */
        g_shift_left = !released;
        return;
    case 0x36u: /* right shift */
        g_shift_right = !released;
        return;
    case 0x1Du: /* left Ctrl */
        g_ctrl = !released;
        return;
    case 0x3Au: /* Caps Lock: a make toggles it, the break does nothing */
        if (!released) {
            g_caps = !g_caps;
        }
        return;
    default:
        break;
    }

    if (released) {
        return; /* the break of a key that was already typed */
    }

    /* The interface owns the keys while it is open: the characters of the
     * console are not typed by a person who is driving a focus. */
    if (g_capture) {
        return;
    }

    character = key_translate(scancode);
    if (character != 0) {
        key_push((k_u8_t)character);
    }
}

void port_x86_64_irq_keyboard(k_u64_t *frame)
{
    K_UNUSED(frame);

    k_u8_t status = port_inb(PS2_STATUS);

    /* The line is level triggered and the byte that raised it is the one to
     * take: a spurious interrupt has nothing in the buffer and is answered
     * with the EOI alone. */
    if ((status & PS2_STATUS_OUTPUT) != 0) {
        key_handle(port_inb(PS2_DATA));
    }

    /*
     * The line may have arrived through the I/O APIC of another core or
     * through the 8259 of the core of the boot: the port knows which chip
     * raised it and answers that one (ADR 0012). The count is per core,
     * because a run that moves the line has to see it arrive where it was
     * sent.
     */
    port_x86_64_irq_note(PORT_X86_64_IRQ_KEYBOARD);
    port_x86_64_irq_eoi();
}

static bool ps2_wait_input_empty(void)
{
    k_u32_t guard;

    for (guard = 0; guard < PS2_TIMEOUT; guard++) {
        if ((port_inb(PS2_STATUS) & PS2_STATUS_INPUT) == 0) {
            return true;
        }
    }

    return false;
}

static bool ps2_wait_output_full(void)
{
    k_u32_t guard;

    for (guard = 0; guard < PS2_TIMEOUT; guard++) {
        if ((port_inb(PS2_STATUS) & PS2_STATUS_OUTPUT) != 0) {
            return true;
        }
    }

    return false;
}

static bool ps2_read_config(k_u8_t *config)
{
    if (!ps2_wait_input_empty()) {
        return false;
    }

    port_outb(PS2_STATUS, PS2_CMD_READ_CONFIG);

    if (!ps2_wait_output_full()) {
        return false;
    }

    *config = port_inb(PS2_DATA);
    return true;
}

static bool ps2_write_config(k_u8_t config)
{
    if (!ps2_wait_input_empty()) {
        return false;
    }

    port_outb(PS2_STATUS, PS2_CMD_WRITE_CONFIG);

    if (!ps2_wait_input_empty()) {
        return false;
    }

    port_outb(PS2_DATA, config);
    return true;
}

void port_x86_64_ps2_init(void)
{
    k_u8_t config;
    k_u32_t guard;

    /* A byte the firmware left in the buffer is not a key of the user, and it
     * would be read as one if the line were opened before the flush. */
    for (guard = 0; guard < PS2_TIMEOUT; guard++) {
        if ((port_inb(PS2_STATUS) & PS2_STATUS_OUTPUT) == 0) {
            break;
        }
        (void)port_inb(PS2_DATA);
    }

    /* The first port on: a keyboard nobody listens to still talks, and the
     * interrupt comes when pic_init() opens the line of IRQ1. */
    if (ps2_wait_input_empty()) {
        port_outb(PS2_STATUS, PS2_CMD_ENABLE_FIRST);
    }

    if (!ps2_read_config(&config)) {
        return;
    }

    /* The interrupt of the first port on and its clock running: the clock is
     * what stops the device when a byte is not read and the controller wants
     * to keep a second one waiting. */
    config |= PS2_CONFIG_IRQ1;
    config &= (k_u8_t)~PS2_CONFIG_CLOCK1;
    (void)ps2_write_config(config);
}
