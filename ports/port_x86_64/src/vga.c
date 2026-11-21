/*
 * Micro OS — the VGA text console of the x86-64 port.
 *
 * The screen of a PC in the mode the firmware left it in is 80 columns by 25
 * rows of cells at 0xB8000, two bytes each: the character and an attribute.
 * The kernel does not program a video mode and does not touch the palette:
 * the mode 3 of the VGA is the one QEMU and every PC has had since 1987, and
 * the text is enough for a console that a person types into. What this file
 * adds to that memory is the state a screen needs and a buffer does not: the
 * position of the cursor, the scroll when the last line is full, and the
 * three control characters a terminal is expected to obey.
 *
 * The bytes come here through k_port_write(), which sends them to COM1 as
 * well: one line of the kernel is one line in the window and one line in the
 * serial log, and neither sink knows about the other.
 */
#include "port_x86_64_console.h"

#include "port_x86_64.h"

#define VGA_BASE 0xB8000u
#define VGA_COLUMNS 80u
#define VGA_ROWS 25u
#define VGA_CELLS (VGA_COLUMNS * VGA_ROWS)

/*
 * Light grey on black: the colours of the terminal the serial console looks
 * like, so that the window and the log are the same picture.
 */
#define VGA_ATTR 0x07u

/* One cell of the video memory: the character in the low byte of the word and
 * the colour in the high one, which is the order the VGA has read since the
 * mode 7 of the monochrome adapter. */
static k_u16_t vga_cell(char character, k_u8_t attribute)
{
    return (k_u16_t)((k_u16_t)(k_u8_t)character | ((k_u16_t)attribute << 8));
}

/* The tab stops of the terminal: every eight columns, from the left edge. */
#define VGA_TAB 8u

/* The cursor of the hardware lives in a register of the cathode ray tube
 * controller, addressed by an index port and read or written by a data port. */
#define CRTC_INDEX 0x3D4u
#define CRTC_DATA 0x3D5u
#define CRTC_CURSOR_START 0x0Au
#define CRTC_CURSOR_END 0x0Bu
#define CRTC_CURSOR_LOW 0x0Fu
#define CRTC_CURSOR_HIGH 0x0Eu

/* The shape of the cursor: the last two of the sixteen scan lines of a cell,
 * which is the underline of the mode 3 the firmware leaves behind. The
 * registers are written anyway, because a firmware that hid the cursor would
 * leave the window without one and the position of the next character is
 * the one thing a person looking at a prompt needs to see. */
#define CURSOR_FIRST_LINE 0x0Eu
#define CURSOR_LAST_LINE 0x0Fu

static volatile k_u16_t *const g_vga = (volatile k_u16_t *)(uintptr_t)VGA_BASE;

static k_u32_t g_row;
static k_u32_t g_col;

/*
 * The one escape sequence the console of the kernel writes is the pair of the
 * `clear` command of the shell: ESC[2J (erase the screen) and ESC[H (home the
 * cursor). A window is not a terminal and the bytes are not for a terminal
 * emulator, so the port reads the sequences itself: the state below is the
 * whole parser - text, after ESC, inside a CSI until the final byte - and a
 * sequence of any other kind is dropped instead of being printed.
 */
#define VGA_TEXT 0u
#define VGA_ESCAPE 1u
#define VGA_CSI 2u

static k_u8_t g_escape = VGA_TEXT;

/*
 * The text of the kernel is UTF-8 and a cell of the screen is one byte of the
 * code page the firmware installed, so a character of more than one byte has
 * to be translated or dropped. The one the kernel writes is the em dash of
 * the banner ("board: ... — ..."), and the ASCII form of it is the hyphen the
 * documentation uses; a byte above 0x7F that belongs to any other character
 * is dropped, because a byte of a UTF-8 sequence is not a letter of the code
 * page and shows as a picture nobody wrote.
 */
#define VGA_UTF8_NONE 0u
#define VGA_UTF8_E2 1u
#define VGA_UTF8_E2_80 2u

static k_u8_t g_utf8 = VGA_UTF8_NONE;

static void cursor_update(void)
{
    k_u16_t position = (k_u16_t)(g_row * VGA_COLUMNS + g_col);

    port_outb(CRTC_INDEX, CRTC_CURSOR_LOW);
    port_outb(CRTC_DATA, (k_u8_t)(position & 0xFFu));
    port_outb(CRTC_INDEX, CRTC_CURSOR_HIGH);
    port_outb(CRTC_DATA, (k_u8_t)(position >> 8));
}

/* The screen is one memory, so a scroll is a move of its rows and a blank
 * last one. The loop stops one row short of the end on purpose: the source
 * of the last move is the last row, which the second loop fills. */
static void scroll(void)
{
    k_u32_t i;

    for (i = 0; i < VGA_CELLS - VGA_COLUMNS; i++) {
        g_vga[i] = g_vga[i + VGA_COLUMNS];
    }

    for (; i < VGA_CELLS; i++) {
        g_vga[i] = vga_cell(' ', VGA_ATTR);
    }
}

static void next_line(void)
{
    g_col = 0;
    g_row++;

    if (g_row >= VGA_ROWS) {
        /* The lines above the last one are gone: the screen scrolls up by a
         * row, which is what a terminal does when its bottom is reached. */
        scroll();
        g_row = VGA_ROWS - 1u;
    }
}

/* One character in the cell of the cursor, then the cursor moves on. */
static void put_char(char character)
{
    g_vga[g_row * VGA_COLUMNS + g_col] = vga_cell(character, (k_u8_t)VGA_ATTR);

    g_col++;
    if (g_col >= VGA_COLUMNS) {
        next_line();
    }
}

static void backspace(void)
{
    if (g_col > 0) {
        g_col--;
    } else if (g_row > 0) {
        /* The first column of a line: the character before it is the last one
         * of the line above, which is where a terminal puts the cursor. */
        g_row--;
        g_col = VGA_COLUMNS - 1u;
    } else {
        return;
    }

    g_vga[g_row * VGA_COLUMNS + g_col] = vga_cell(' ', VGA_ATTR);
}

void port_x86_64_vga_init(void)
{
    k_u32_t i;

    for (i = 0; i < VGA_CELLS; i++) {
        g_vga[i] = vga_cell(' ', VGA_ATTR);
    }

    g_row = 0;
    g_col = 0;
    g_escape = VGA_TEXT;
    g_utf8 = VGA_UTF8_NONE;

    port_outb(CRTC_INDEX, CRTC_CURSOR_START);
    port_outb(CRTC_DATA, CURSOR_FIRST_LINE);
    port_outb(CRTC_INDEX, CRTC_CURSOR_END);
    port_outb(CRTC_DATA, CURSOR_LAST_LINE);

    cursor_update();
}

void port_x86_64_vga_write(const char *data, k_size_t len)
{
    k_size_t i;

    if (data == NULL) {
        return;
    }

    for (i = 0; i < len; i++) {
        char character = data[i];

        if (g_escape == VGA_ESCAPE) {
            /* The byte after ESC says what kind of a sequence follows; the
             * screen only knows the control sequences of the CSI form. */
            g_escape = (character == '[') ? VGA_CSI : VGA_TEXT;
            continue;
        }

        if (g_escape == VGA_CSI) {
            if (character < '@' || character > '~') {
                continue; /* a parameter or an intermediate byte */
            }

            g_escape = VGA_TEXT;

            if (character == 'J') {
                port_x86_64_vga_init(); /* ESC[2J: the cells and the cursor */
            } else if (character == 'H') {
                g_row = 0;
                g_col = 0;
            }
            /* Any other final byte ends the sequence and leaves no trace. */
            continue;
        }

        if (character == 0x1B) {
            g_escape = VGA_ESCAPE;
            continue;
        }

        if (g_utf8 == VGA_UTF8_E2) {
            g_utf8 = ((k_u8_t)character == 0x80u) ? VGA_UTF8_E2_80 : VGA_UTF8_NONE;
            continue;
        }

        if (g_utf8 == VGA_UTF8_E2_80) {
            /* E2 80 94 is the em dash; the other characters of the family
             * (the quotes, the ellipsis) are not ones the kernel writes, and
             * the screen has no glyph for them. */
            if ((k_u8_t)character == 0x94u) {
                put_char('-');
            }
            g_utf8 = VGA_UTF8_NONE;
            continue;
        }

        if ((k_u8_t)character == 0xE2u) {
            g_utf8 = VGA_UTF8_E2;
            continue;
        }

        switch (character) {
        case '\n':
            next_line();
            break;
        case '\r':
            g_col = 0;
            break;
        case '\t': {
            /* To the next stop, not by eight: a tab at the column 3 of the
             * line lands on the column 8 of the same line. The stops are the
             * ones of the terminal, so the text of a file lines up the same
             * way it does on the serial console. */
            k_u32_t stop = (g_col / VGA_TAB + 1u) * VGA_TAB;

            while (g_col < stop) {
                put_char(' ');
            }
            break;
        }
        case '\b':
            backspace();
            break;
        default:
            if ((k_u8_t)character < 0x20u || (k_u8_t)character >= 0x7Fu) {
                break; /* a control character or a byte the font has no letter for */
            }
            put_char(character);
            break;
        }
    }

    /* After the whole call and not after every character: the cursor is one
     * register of the controller, and a line of text is one write to it. */
    cursor_update();
}
