/*
 * Micro OS — the PS/2 keyboard of the x86-64 port.
 *
 * The keyboard of the PC hangs on the first line of the 8259 and speaks the
 * scancode set 1: the 8042 of the machine translates the set 2 of the device
 * into the bytes a keyboard of the 1980s made, and those are the bytes the
 * monitor of QEMU sends too. The handler runs with the interrupts off and
 * does the least it can: read one byte of the data port, translate it, put
 * the character in a ring, acknowledge the chip. A task takes the characters
 * out through k_port_console_poll() and k_port_console_getchar().
 *
 * The map below is the US layout, which is the one the scancodes of the
 * machine carry: letters, digits, the punctuation of a command line and the
 * three keys a line needs (Enter, Backspace and Ctrl+C). What is missing is
 * missing on purpose, and the documentation of the port says so: the E0
 * sequences (the arrows, the keypad of the second form, the right Ctrl) are
 * dropped whole, the function keys type nothing, and there is no repeat of
 * a held key of its own because the device repeats it.
 *
 * Since ADR 0028 every key of the set has a name as well and the handler
 * pushes an event beside the character (`/dev/input0`): the Tab, the function
 * keys and the releases have no character, and an interface of a program
 * needs exactly the keys the console cannot carry.
 */
#include "port_x86_64_console.h"

#include "kernel/input.h"
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

#define PIC1_COMMAND 0x20u
#define PIC_EOI 0x20u

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

/* An E0 (or E1) sequence is two or more bytes, and none of the keys behind
 * it is the console's: the flag drops the byte that follows it. */
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

/* The name of the key a scancode of the set 1 means, or K_INPUT_KEY_NONE when
 * it is not a key of the contract. The table is the one of the characters
 * above in another shape: a key that types nothing (a modifier, a function
 * key) has no character, and a key that types something still has a name. */
static k_u16_t key_code(k_u8_t scancode)
{
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
    default:
        return K_INPUT_KEY_NONE;
    }
}

/*
 * One byte of the device. The break codes are the make codes with the bit 7
 * set, so a key that goes up is the same number: the modifiers are tracked
 * on both edges, and every other key only acts when it goes down.
 *
 * Every byte is also an event of `/dev/input0` (ADR 0028), both edges
 * included: an interface that draws a key being held needs the release, which
 * the character stream of the console has no way to say.
 */
static void key_handle(k_u8_t code)
{
    bool released;
    k_u8_t scancode;
    k_u16_t key;
    char character;

    if (code == 0xE0u || code == 0xE1u) {
        g_skip_next = true;
        return;
    }

    if (g_skip_next) {
        g_skip_next = false;
        return;
    }

    released = (code & 0x80u) != 0;
    scancode = code & 0x7Fu;
    key = key_code(scancode);

    if (key != K_INPUT_KEY_NONE) {
        event_push(key, released ? INPUT_VALUE_RELEASE : INPUT_VALUE_PRESS);
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

    port_outb(PIC1_COMMAND, PIC_EOI);
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
