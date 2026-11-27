/*
 * Micro OS — the PS/2 keyboard of the x86-64 port.
 *
 * The keyboard of the PC hangs on the first line of the interrupt controller -
 * the 8259 of the legacy chip, or the I/O APIC that routes the line to any
 * core (ADR 0012) - and speaks one of the three scancode sets of the machine
 * (kernel/input_scancode.h). The controller of the motherboard, the 8042,
 * translates the set 2 of the device into the bytes a keyboard of the 1980s
 * made, and a machine whose firmware leaves that translation on shows the set
 * 1 whatever the keyboard is - the keyboard of QEMU, whose 8042 translates,
 * and the one of every PC of the last thirty years.
 *
 * The laptop of 1.8 shows the other half: its firmware turns the translation
 * off, its keyboard speaks the set 2, and every byte of it read as the set 1
 * is another key (or no key at all) - the machine boots, the prompt arrives,
 * and the line fills with keys nobody sees. So the boot asks the keyboard
 * which set it speaks (`0xF0 0x00`, the command of the specification) and the
 * controller whether it translates (bit 6 of its configuration byte), and the
 * line `ps2: the keyboard speaks set N` of the boot is the answer of the
 * machine a person is looking at.
 *
 * The handler runs with the interrupts off and does the least it can: read one
 * byte of the data port, hand it to the decoder of the kernel
 * (kernel/input_scancode.c), put the character in a ring, acknowledge the
 * chip. A task takes the characters out through k_port_console_poll() and
 * k_port_console_getchar().
 *
 * The map below is the US layout, and it is written in the addresses of the
 * set 1: the decoder of the kernel gives every key of every set as its address
 * of the set 1, so a keyboard of the set 2 types the same `w` as one of the
 * set 1. The layout is the letters, the digits, the punctuation of a command
 * line and the keys a line needs (Enter, Backspace and Ctrl+C). The E0
 * sequences arrived with the interface of 1.4.0: the arrows, Home, End,
 * Insert and Delete have a name and an event now (`kernel/input_set1.h`), the
 * right Ctrl is the modifier of the same form and the Enter and the slash of
 * the keypad still type their character for the shell. The function keys type
 * nothing and there is no repeat of a held key of its own because the device
 * repeats it.
 *
 * Since ADR 0028 every key of the set has a name as well and the handler
 * pushes an event beside the character (`/dev/input0`): the Tab, the function
 * keys and the releases have no character, and an interface of a program
 * needs exactly the keys the console cannot carry.
 */
#include "port_x86_64_console.h"

#include "kernel/console.h"
#include "kernel/input.h"
#include "kernel/input_ps2_mouse.h"
#include "kernel/input_scancode.h"
#include "kernel/input_set1.h"
#include "kernel/log.h"
#include "kernel/port/lock.h"
#include "kernel/port/time.h"
#include "port_x86_64.h"
#include "port_x86_64_input.h"

/* The controller of the keyboard, the 8042: a data port and a status port. */
#define PS2_DATA 0x60u
#define PS2_STATUS 0x64u
#define PS2_STATUS_OUTPUT 0x01u /* a byte waits in the data port */
#define PS2_STATUS_INPUT 0x02u  /* the controller is still taking a command */
#define PS2_STATUS_AUX 0x20u    /* the byte that waits is the mouse's */

#define PS2_CMD_READ_CONFIG 0x20u
#define PS2_CMD_WRITE_CONFIG 0x60u
#define PS2_CMD_ENABLE_FIRST 0xAEu
#define PS2_CONFIG_IRQ1 0x01u   /* the first port raises IRQ1 */
#define PS2_CONFIG_CLOCK1 0x10u /* set: the clock of the first port is off */

/*
 * The pointer of the machine (1.7.0). The mouse of the PC is the second
 * device of the same 8042: the controller takes a byte of it with the command
 * 0xD4, the data of the two devices arrive in one port, and the status byte
 * says which of them the byte belongs to (bit 5). Its line is IRQ12, which is
 * the fourth line of the slave of the 8259 - the cascade of the second chip
 * and the line itself are the two bits the port opens for it.
 */
#define PS2_STATUS_AUX 0x20u       /* set: the byte waits for the second port */
#define PS2_CMD_ENABLE_AUX 0xA8u   /* the second port on */
#define PS2_CMD_WRITE_AUX 0xD4u    /* the next byte of 0x60 is the mouse's */
#define PS2_CONFIG_IRQ12 0x02u     /* the second port raises IRQ12 */
#define PS2_CONFIG_CLOCK2 0x20u    /* set: the clock of the second port is off */
#define PS2_MOUSE_DEFAULTS 0xF6u   /* the mouse starts from its defaults */
#define PS2_MOUSE_ENABLE 0xF4u     /* and reports its movements */
#define PS2_MOUSE_SET_SAMPLE 0xF3u /* the rate of the report, in samples a second */
#define PS2_MOUSE_GET_ID 0xF2u     /* the kind of the device: 0 plain, 3 wheel */
#define PS2_ACK 0xFAu              /* the answer of every command of a device */

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

/*
 * The events of the pointer are a ring of their own (1.7.0). The keyboard may
 * be moved to another core with `smp irq keyboard` while the mouse stays on
 * the core of the boot, so the two handlers must not write one ring: a ring
 * with one writer per core is the rule the keyboard already follows, and the
 * reader merges the two in the order of the two devices.
 */
static volatile struct input_event g_pointer[PS2_EVENT_RING];
static volatile k_u32_t g_pointer_head; /* written by IRQ12 */
static volatile k_u32_t g_pointer_tail; /* written and read by the tasks */

/*
 * The packet of the mouse: three bytes for a plain mouse and four for an
 * IntelliMouse with a wheel. The decoder is pure (kernel/input_ps2_mouse.h);
 * the port feeds it bytes, counts its overflows and pushes its events into
 * the pointer ring.
 */
static struct k_input_ps2_mouse_state g_ps2_mouse;
static bool g_overflow_shown;

/* The pointer is there when the controller answered the commands of its own
 * (a machine without a mouse has nothing on the second port). */
static bool g_pointer_present;

/* While a program holds the keyboard, the keys are events and not characters:
 * a `q` of an interface must not become a line of the shell that waits for
 * the interface to end. */
static bool g_capture;

/* The state of the modifiers, as the make and the break codes left it. */
static bool g_shift_left;
static bool g_shift_right;
static bool g_ctrl;
static bool g_caps;

/*
 * The set of the keyboard: the set of the bytes the handler reads (one of
 * K_INPUT_SCANCODE_SET*) and the set the device said it is in, which is the
 * number the line of the boot prints. The decoder is the state of the stream -
 * the prefixes of an extended key, of a break and of the Pause - and it gives
 * every key as an address of the set 1, which is the language of the map
 * below and of the table of the events.
 */
static struct k_input_decoder g_decoder = {.set = K_INPUT_SCANCODE_SET1};
static k_u8_t g_set = K_INPUT_SCANCODE_SET1;
/* True when the port told the device to speak the set 1 and it answered. */
static bool g_set_ordered;
static k_u8_t g_device_set;

k_u8_t port_x86_64_ps2_set(void)
{
    return g_set;
}

bool port_x86_64_ps2_set_ordered(void)
{
    return g_set_ordered;
}

k_u8_t port_x86_64_ps2_device_set(void)
{
    return g_device_set;
}

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

    /* A task that waits for the console in `poll` has its answer now. */
    k_console_input_notify();
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

/* One event into the ring of the pointer, the same rule of a full ring. */
static void pointer_push(k_u16_t code, k_u16_t value)
{
    k_u32_t next = (g_pointer_head + 1u) % PS2_EVENT_RING;

    if (next == g_pointer_tail) {
        return;
    }

    g_pointer[g_pointer_head].ms = (k_u32_t)k_port_uptime_ms();
    g_pointer[g_pointer_head].code = code;
    g_pointer[g_pointer_head].value = value;
    g_pointer_head = next;
}

/*
 * One byte of the mouse. The decoder of the kernel turns it into the events
 * of a packet when a whole one is ready; the port only owns the ring and the
 * line of the log that names the first overflow.
 */
static void mouse_byte(k_u8_t byte)
{
    struct k_input_hid_event events[8];
    k_size_t count;
    k_size_t i;

    count = k_input_ps2_mouse_feed(&g_ps2_mouse, byte, events, K_ARRAY_SIZE(events));

    for (i = 0; i < count; i++) {
        pointer_push(events[i].key, events[i].value);
    }

    if ((g_ps2_mouse.overflow_x != 0u || g_ps2_mouse.overflow_y != 0u) && !g_overflow_shown) {
        g_overflow_shown = true;
        kwarn("input: the mouse overflowed its packet (%u x, %u y), the packet is dropped",
              (unsigned)g_ps2_mouse.overflow_x, (unsigned)g_ps2_mouse.overflow_y);
    }
}

k_u32_t port_x86_64_input_overflow(bool vertical)
{
    return vertical ? g_ps2_mouse.overflow_y : g_ps2_mouse.overflow_x;
}

bool port_x86_64_input_pointer(void)
{
    return g_pointer_present;
}

bool port_x86_64_input_event(struct input_event *out)
{
    k_u32_t tail = g_event_tail;

    if (out == NULL) {
        return false;
    }

    if (tail != g_event_head) {
        out->ms = g_events[tail].ms;
        out->code = g_events[tail].code;
        out->value = g_events[tail].value;
        g_event_tail = (tail + 1u) % PS2_EVENT_RING;
        return true;
    }

    tail = g_pointer_tail;

    if (tail == g_pointer_head) {
        return false;
    }

    out->ms = g_pointer[tail].ms;
    out->code = g_pointer[tail].code;
    out->value = g_pointer[tail].value;
    g_pointer_tail = (tail + 1u) % PS2_EVENT_RING;

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
    g_pointer_tail = g_pointer_head;
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
 * One byte of the device: the decoder of the kernel turns the stream of the set
 * of this keyboard into the address of a key of the set 1 and its edge, and
 * this file does the rest - the ring of the characters, the ring of the events
 * and the state of the modifiers and of Caps Lock.
 *
 * Every key is also an event of `/dev/input0` (ADR 0028), both edges
 * included: an interface that draws a key being held needs the release, which
 * the character stream of the console has no way to say.
 *
 * The prefix 0xE0 is the second form of the key and not a byte to drop
 * (1.4.0): the arrows, Home, End, Insert and Delete have no address of the
 * first 128, and a program of a text already asks for them. The right Ctrl is
 * the modifier of that form, and the Enter and the slash of the keypad still
 * type their character for the shell. In the set 3 the same keys have an
 * address of one byte, and the decoder gives them the same second form.
 */
static void key_handle(k_u8_t byte)
{
    struct k_input_code code;
    k_u16_t key;
    char character;

    /*
     * The claim of a controller about the set of its bytes is not always true:
     * a firmware of a laptop keeps the bit of the translation set and delivers
     * the set 2 of the device all the same, and a keyboard read in the wrong
     * set is a line of control characters and no letter on the screen. The two
     * streams cannot be confused for long: a release of a key is the make code
     * with the bit 7 set in the set 1, and the prefix 0xF0 in the set 2, and a
     * byte of the other set is impossible in each. The first such byte turns
     * the decoder over, once, with a line in the log.
     */
    if (g_decoder.set == K_INPUT_SCANCODE_SET1 && (byte == 0xF0u || byte == 0xF1u)) {
        g_set = K_INPUT_SCANCODE_SET2;
        k_input_decoder_init(&g_decoder, g_set);
        kinfo("ps2: the bytes are the set 2 after all, the decoder follows them");
        return;
    }

    if (g_decoder.set == K_INPUT_SCANCODE_SET2 && (byte & 0x80u) != 0u && byte != 0xF0u &&
        byte != 0xE0u && byte != 0xE1u && byte != 0xFAu && byte != 0xFEu) {
        g_set = K_INPUT_SCANCODE_SET1;
        k_input_decoder_init(&g_decoder, g_set);
        kinfo("ps2: the bytes are the set 1 after all, the decoder follows them");
        return;
    }

    if (!k_input_decoder_feed(&g_decoder, byte, &code)) {
        return; /* a prefix, a byte of the Pause, or an address of no key */
    }

    key = k_input_set1_key(code.code, code.extended);

    if (key != K_INPUT_KEY_NONE) {
        event_push(key, code.released ? INPUT_VALUE_RELEASE : INPUT_VALUE_PRESS);
    }

    if (code.extended) {
        /* The right Ctrl is a modifier of the second form; the right Alt and
         * the rest of the block are events and no characters of the console. */
        if (code.code == 0x1Du) {
            g_ctrl = !code.released;
            return;
        }

        if (code.released || g_capture) {
            return;
        }

        /* The Enter and the slash of the keypad are the keys the shell types. */
        if (code.code == 0x1Cu) {
            key_push('\r');
        } else if (code.code == 0x35u) {
            key_push('/');
        }
        return;
    }

    switch (code.code) {
    case 0x2Au: /* left shift */
        g_shift_left = !code.released;
        return;
    case 0x36u: /* right shift */
        g_shift_right = !code.released;
        return;
    case 0x1Du: /* left Ctrl */
        g_ctrl = !code.released;
        return;
    case 0x3Au: /* Caps Lock: a make toggles it, the break does nothing */
        if (!code.released) {
            g_caps = !g_caps;
        }
        return;
    default:
        break;
    }

    if (code.released) {
        return; /* the break of a key that was already typed */
    }

    /* The interface owns the keys while it is open: the characters of the
     * console are not typed by a person who is driving a focus. */
    if (g_capture) {
        return;
    }

    character = key_translate(code.code);
    if (character != 0) {
        key_push((k_u8_t)character);
    }
}

/*
 * The bytes the controller holds, taken until the buffer is empty. Its buffer
 * is one for the two devices of the machine (1.7.0): the status byte says
 * whether the byte is a key or a motion, so one body serves both interrupt
 * lines and a byte of the other device that arrived at the same moment is
 * not lost.
 */
/*
 * The first bytes of the keyboard, kept as they arrived: a machine whose keys
 * are rubbish (a laptop of 2026, whose EC speaks a set the driver guessed
 * wrong) answers with the bytes themselves instead of a sentence about them.
 * Sixteen bytes, one line of the log, once.
 */
#define PS2_TRACE_BYTES 16u
static k_u8_t g_trace[PS2_TRACE_BYTES];
static k_u32_t g_trace_count;

static void ps2_trace(k_u8_t byte)
{
    char line[PS2_TRACE_BYTES * 3u + 24u];
    k_size_t used = 0;
    k_u32_t i;

    if (g_trace_count >= PS2_TRACE_BYTES) {
        return;
    }

    g_trace[g_trace_count++] = byte;

    if (g_trace_count < PS2_TRACE_BYTES) {
        return;
    }

    {
        static const char head[] = "ps2: the first bytes: ";
        static const char hex[] = "0123456789abcdef";

        for (i = 0; head[i] != '\0' && used + 1u < sizeof(line); i++) {
            line[used++] = head[i];
        }

        for (i = 0; i < PS2_TRACE_BYTES && used + 3u < sizeof(line); i++) {
            line[used++] = hex[(g_trace[i] >> 4) & 0x0Fu];
            line[used++] = hex[g_trace[i] & 0x0Fu];
            line[used++] = ' ';
        }

        line[used] = '\0';
    }

    kinfo("%s", line);
}

static void ps2_take_bytes(void)
{
    int guard;

    for (guard = 0; guard < 16; guard++) {
        k_u8_t status = port_inb(PS2_STATUS);
        k_u8_t byte;

        if ((status & PS2_STATUS_OUTPUT) == 0) {
            return;
        }

        byte = port_inb(PS2_DATA);

        if ((status & PS2_STATUS_AUX) != 0) {
            mouse_byte(byte);
        } else {
            ps2_trace(byte);
            key_handle(byte);
        }
    }
}

void port_x86_64_irq_keyboard(k_u64_t *frame)
{
    K_UNUSED(frame);

    ps2_take_bytes();

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

/*
 * The pointer: the twelfth line of the machine and the second device of the
 * 8042 (1.7.0). The byte is read by the same body as the keyboard's, because
 * the buffer of the controller is one and the status byte is what says whose
 * byte it is: a keyboard byte that arrived while the mouse raised its line is
 * a key and not a motion.
 *
 * The line has no counter of its own in `smp irq`: the report of that command
 * is the two lines of ADR 0012, and the mouse is a device of the interface
 * and not a line the scheduler moves. It is counted with the keyboard, which
 * is the number a person reads to see that the controller is alive.
 */
void port_x86_64_irq_mouse(k_u64_t *frame)
{
    K_UNUSED(frame);

    ps2_take_bytes();
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

/* The mouse of the second port, armed at the end of the controller's init. */
static void ps2_mouse_init(void);

/* --- which set the keyboard speaks ---------------------------------------- */

/*
 * The command of the *keyboard*, and not of the controller: 0xF0 followed by
 * an argument, and the argument 0 asks which set the device is in. The answer
 * is 0x01, 0x02 or 0x03 - or, when the controller translates the bytes of the
 * keyboard, the translation of that byte (0x43, 0x41 or 0x3F), because the
 * translation is a fact of the controller and not of the device.
 */
#define PS2_KBD_CMD 0xF0u
#define PS2_KBD_ACK 0xFAu
#define PS2_KBD_RESEND 0xFEu

/* Bit 6 of the configuration byte of the controller: the controller translates
 * the set 2 of the keyboard into the set 1. */
#define PS2_CONFIG_TRANSLATE 0x40u

/* The attempts of one exchange of a command: a device may answer a resend (a
 * byte of the sequence arrived garbled) and the exchange starts again. */
#define PS2_KBD_ATTEMPTS 3u

/* The waits of the exchange with the device are short: a keyboard answers in
 * microseconds, and a machine whose controller holds no keyboard is a machine
 * whose boot is not going to lose a second to the question. */
#define PS2_KBD_TIMEOUT 20000u

/* One byte of the *keyboard*, waited for by the boot. The line of the keyboard
 * is still masked when this runs, so the answer waits in the buffer of the
 * controller for the caller that asks; -1 when nothing comes.
 *
 * A byte of the mouse is not an answer of the keyboard and is dropped: the
 * status port says which of the two devices put it there, and a hand on the
 * touchpad during the boot must not turn into a set of scancodes. It is the
 * one byte of the second port this file ever reads, and the device is left as
 * it was. */
static int ps2_read_byte(void)
{
    k_u32_t guard;

    for (guard = 0; guard < PS2_KBD_TIMEOUT; guard++) {
        k_u8_t status = port_inb(PS2_STATUS);

        if ((status & PS2_STATUS_OUTPUT) == 0u) {
            continue;
        }

        if ((status & PS2_STATUS_AUX) != 0u) {
            (void)port_inb(PS2_DATA);
            continue;
        }

        return (int)port_inb(PS2_DATA);
    }

    return -1;
}

/* One byte to the device. */
static bool ps2_write_byte(k_u8_t byte)
{
    if (!ps2_wait_input_empty()) {
        return false;
    }

    port_outb(PS2_DATA, byte);

    return true;
}

/* A command of the keyboard, its argument, and the ACK of each: the answer of
 * the device, or -1 when it gives none this port understands. */
static int ps2_kbd_command(k_u8_t command, k_u8_t argument)
{
    k_u32_t attempt;

    for (attempt = 0; attempt < PS2_KBD_ATTEMPTS; attempt++) {
        int ack;

        if (!ps2_write_byte(command) || (ack = ps2_read_byte()) != (int)PS2_KBD_ACK) {
            /* A resend, or nothing at all: the buffer is emptied and the
             * command is said again. */
            while (ps2_wait_output_full()) {
                (void)port_inb(PS2_DATA);
            }
            continue;
        }

        if (!ps2_write_byte(argument) || (ack = ps2_read_byte()) != (int)PS2_KBD_ACK) {
            while (ps2_wait_output_full()) {
                (void)port_inb(PS2_DATA);
            }
            continue;
        }

        return ps2_read_byte();
    }

    return -1;
}

/* The two sets behind one answer of the command: the set of the device and the
 * set of the bytes the handler reads. The translation of the controller makes
 * the second one the set 1 whatever the first one is. False when the answer is
 * not one this port knows. */
static bool ps2_sets_of(int answer, k_u8_t *device, k_u8_t *stream)
{
    switch (answer) {
    case 0x01: /* the device is in the set 1 */
    case 0x43: /* and the controller translates that answer, which changes nothing */
        *device = K_INPUT_SCANCODE_SET1;
        *stream = K_INPUT_SCANCODE_SET1;
        return true;
    case 0x02: /* the device is in the set 2, and the controller does not translate */
        *device = K_INPUT_SCANCODE_SET2;
        *stream = K_INPUT_SCANCODE_SET2;
        return true;
    case 0x03:
        *device = K_INPUT_SCANCODE_SET3;
        *stream = K_INPUT_SCANCODE_SET3;
        return true;
    case 0x41: /* a device of the set 2 whose bytes the controller translates */
        *device = K_INPUT_SCANCODE_SET2;
        *stream = K_INPUT_SCANCODE_SET1;
        return true;
    case 0x3F: /* the same, for the set 3 */
        *device = K_INPUT_SCANCODE_SET3;
        *stream = K_INPUT_SCANCODE_SET1;
        return true;
    default:
        return false;
    }
}

/*
 * Which set the keyboard of this machine speaks.
 *
 * Two things decide it and they are different: the set of the device and the
 * set of the bytes the handler reads. The bit of the translation of the
 * controller is the truth about the bytes and it is read first, because a
 * machine whose firmware left it on shows the set 1 whatever the keyboard is.
 * The question to the device comes after it: it is harmless, it is what the
 * line of the boot prints, and a device that does not answer leaves the answer
 * to the controller - the set 1 of a controller that translates, and the set 2
 * of one that does not, because a controller with that bit is a machine of the
 * AT and the keyboard of the AT is the set 2. (An XT has no configuration byte
 * to read at all: the boot of such a machine stops before this line.)
 */
static void ps2_detect(void)
{
    k_u8_t config;
    k_u8_t device = 0;
    k_u8_t stream = K_INPUT_SCANCODE_SET1;
    int answer;

    g_set = K_INPUT_SCANCODE_SET1;
    g_device_set = 0;

    if (!ps2_read_config(&config)) {
        return;
    }

    answer = ps2_kbd_command(PS2_KBD_CMD, 0x00u);

    if (!ps2_sets_of(answer, &device, &stream)) {
        /* A device that did not say (a firmware that does not pass the
         * command, a keyboard that is not one of the three sets): the
         * controller is the one fact left, and a controller that does not
         * translate is a controller whose keyboard speaks the set 2. */
        g_set =
            (config & PS2_CONFIG_TRANSLATE) != 0u ? K_INPUT_SCANCODE_SET1 : K_INPUT_SCANCODE_SET2;
        return;
    }

    g_device_set = device;
    g_set = (config & PS2_CONFIG_TRANSLATE) != 0u ? K_INPUT_SCANCODE_SET1 : stream;

    /*
     * With the translation off the wire carries the set of the device, and a
     * keyboard of the set 2 is asked to move to the set 1: one set is simpler
     * than two, and the order is a fact where a claim of a controller is not.
     * With the translation on the order cannot be given - the controller
     * expects the set 2 of the device and would translate a set 1 into
     * nonsense - so the wire stays what the controller makes of it, and the
     * decoder corrects itself on the first key that cannot be of its set (see
     * key_handle).
     */
    if ((config & PS2_CONFIG_TRANSLATE) == 0u && device == K_INPUT_SCANCODE_SET2) {
        if (ps2_kbd_command(PS2_KBD_CMD, 0x01u)) {
            g_device_set = K_INPUT_SCANCODE_SET1;
            g_set = K_INPUT_SCANCODE_SET1;
            g_set_ordered = true;
        }
    }
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

    /*
     * The clock of the first port on before anything is asked of the device:
     * the controller stops a keyboard whose bytes nobody reads, and the answer
     * of the question below has to wait in the buffer for this port and not
     * for an interrupt. The line of IRQ1 stays as the firmware left it until
     * the question is answered.
     */
    if (ps2_read_config(&config) && (config & PS2_CONFIG_CLOCK1) != 0u) {
        (void)ps2_write_config((k_u8_t)(config & (k_u8_t)~PS2_CONFIG_CLOCK1));
    }

    ps2_detect();

    /* The buffer again, after the question: its answer, or the last byte of a
     * sequence the keyboard was in the middle of, is not a key of the person
     * at the machine. */
    for (guard = 0; guard < PS2_TIMEOUT; guard++) {
        if ((port_inb(PS2_STATUS) & PS2_STATUS_OUTPUT) == 0) {
            break;
        }
        (void)port_inb(PS2_DATA);
    }

    if (!ps2_read_config(&config)) {
        return;
    }

    /* The interrupt of the first port on and its clock running: the clock is
     * what stops the device when a byte is not read and the controller wants
     * to keep a second one waiting. The second port is opened the same way
     * (1.7.0): its clock runs, its line is IRQ12, and the mouse itself is
     * told to start from its defaults and report what it measures. */
    config |= PS2_CONFIG_IRQ1 | PS2_CONFIG_IRQ12;
    config &= (k_u8_t) ~(PS2_CONFIG_CLOCK1 | PS2_CONFIG_CLOCK2);
    (void)ps2_write_config(config);

    /* The stream of the device is what the decoder reads from now on. */
    k_input_decoder_init(&g_decoder, g_set);

    ps2_mouse_init();
}

/* --- the mouse of the machine (1.7.0) --------------------------------------- */

/* A byte of the second port: the answer of the controller or of the mouse to
 * a command. The answer of a command is 0xFA and a device that is not there
 * answers nothing, which is what the timeout is for. */
static bool ps2_read_ack(void)
{
    if (!ps2_wait_output_full()) {
        return false;
    }

    return port_inb(PS2_DATA) == PS2_ACK;
}

static bool ps2_mouse_command(k_u8_t command)
{
    if (!ps2_wait_input_empty()) {
        return false;
    }

    port_outb(PS2_STATUS, PS2_CMD_WRITE_AUX);

    if (!ps2_wait_input_empty()) {
        return false;
    }

    port_outb(PS2_DATA, command);

    return ps2_read_ack();
}

/* A command that takes one data byte: the command is ACKed, the data is sent
 * the same way, and the data is ACKed. */
static bool ps2_mouse_command_arg(k_u8_t command, k_u8_t argument)
{
    if (!ps2_mouse_command(command)) {
        return false;
    }

    if (!ps2_wait_input_empty()) {
        return false;
    }

    port_outb(PS2_STATUS, PS2_CMD_WRITE_AUX);

    if (!ps2_wait_input_empty()) {
        return false;
    }

    port_outb(PS2_DATA, argument);

    return ps2_read_ack();
}

/* The device id of the mouse (0xF2): 0 for a plain mouse, 3 for an
 * IntelliMouse with a wheel, 4 for one with five buttons. The answer is -1
 * when the device did not answer. */
static int ps2_mouse_id(void)
{
    if (!ps2_mouse_command(PS2_MOUSE_GET_ID)) {
        return -1;
    }

    if (!ps2_wait_output_full()) {
        return -1;
    }

    return (int)port_inb(PS2_DATA);
}

/*
 * The IntelliMouse magic: set the sample rate to 200, 100, 80 and ask for the
 * id. A plain mouse keeps answering 0; a mouse with a wheel answers 3 and
 * sends a fourth byte in every packet from then on.
 */
static bool ps2_mouse_detect_wheel(void)
{
    int id;

    if (!ps2_mouse_command_arg(PS2_MOUSE_SET_SAMPLE, 200u) ||
        !ps2_mouse_command_arg(PS2_MOUSE_SET_SAMPLE, 100u) ||
        !ps2_mouse_command_arg(PS2_MOUSE_SET_SAMPLE, 80u)) {
        return false;
    }

    id = ps2_mouse_id();
    return id == 3 || id == 4;
}

/*
 * The second port of the 8042: on, with its line and its clock, and the mouse
 * told to report. The call runs before the interrupts are on (the boot arms
 * the two devices before it opens any line), so the bytes read here are the
 * answers of the commands and nothing of the user.
 *
 * A machine without a mouse is a machine whose second port answers nothing:
 * the keyboard keeps working and the desktop of 1.7 has no pointer, which the
 * server of the desktop asks before it draws one.
 */
static void ps2_mouse_init(void)
{
    bool wheel = false;

    k_input_ps2_mouse_init(&g_ps2_mouse, false);

    if (!ps2_wait_input_empty()) {
        return;
    }

    port_outb(PS2_STATUS, PS2_CMD_ENABLE_AUX);

    /* The defaults first: a mouse that kept the scale of somebody else's
     * program would move by a number the desktop did not choose. */
    g_pointer_present = ps2_mouse_command(PS2_MOUSE_DEFAULTS);

    if (g_pointer_present) {
        wheel = ps2_mouse_detect_wheel();
        k_input_ps2_mouse_init(&g_ps2_mouse, wheel);
        g_pointer_present = ps2_mouse_command(PS2_MOUSE_ENABLE);
    }

    /* A byte the mouse left between the answers (a self test of a device
     * that was reset) is not a packet: the buffer is emptied with the
     * interrupts still off, so that the first motion is the first packet. */
    while ((port_inb(PS2_STATUS) & PS2_STATUS_OUTPUT) != 0) {
        (void)port_inb(PS2_DATA);
    }

    if (!g_pointer_present) {
        kwarn("input: the second port of the controller did not answer, the machine has no mouse");
    } else if (wheel) {
        kinfo("ps2: the mouse is an IntelliMouse, the wheel is the fourth byte");
    }
}
