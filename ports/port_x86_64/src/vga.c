/*
 * Micro OS — the text console of the x86-64 port: the fallback screen.
 *
 * The screen of a PC in the mode the firmware left it in is 80 columns by 25
 * rows of cells at 0xB8000, two bytes each: the character and an attribute.
 * The port does not program a video mode here and does not touch the palette:
 * the mode 3 of the VGA is the one QEMU and every PC has had since 1987, and
 * when the machine has no Bochs VBE interface to program (ADR 0023) this text
 * is the whole picture. It is the fallback and not a second console: the state
 * of the dialogue and its escape sequences live in src/console.c, and this
 * file is the screen that puts a cell into the video memory.
 *
 * The attribute of a cell is the one the console computed (kernel/ansi.h): the
 * nibble of the ink and the nibble of the paper of the sixteen colours the
 * mode has had from the beginning. The plain text carries the pair of the
 * `console` system of /etc (ADR 0021) - light grey on black, black on light
 * grey, with the inversion of the file - and a word a command coloured keeps
 * its own; a change of the pair repaints the cells of the pair and leaves the
 * others.
 */
#include "port_x86_64_console.h"

#include "kernel/log.h"
#include "port_x86_64.h"

#define VGA_BASE 0xB8000u
#define VGA_COLUMNS 80
#define VGA_ROWS 25
#define VGA_CELLS (VGA_COLUMNS * VGA_ROWS)

/*
 * Light grey on black: the colours of the terminal the serial console looks
 * like, so that the window and the log are the same picture. The theme of
 * /etc chooses the pair and the console hands it over as an attribute; the
 * value here is the pair of the boot, before the files are read.
 */
#define VGA_ATTR_BOOT 0x07u

static k_u8_t g_theme_attr = VGA_ATTR_BOOT;

/* One cell of the video memory: the character in the low byte of the word and
 * the colour in the high one, which is the order the VGA has read since the
 * mode 7 of the monochrome adapter. */
static k_u16_t vga_cell(char character, k_u8_t attribute)
{
    return (k_u16_t)((k_u16_t)(k_u8_t)character | ((k_u16_t)attribute << 8));
}

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

/* The paper and the ink of an attribute traded: the cursor and the SGR 7. */
static k_u8_t attr_reverse(k_u8_t attr)
{
    return PORT_X86_64_ATTR(PORT_X86_64_ATTR_BG(attr), PORT_X86_64_ATTR_FG(attr));
}

static void text_clear(k_u8_t attr)
{
    int i;

    for (i = 0; i < (int)VGA_CELLS; i++) {
        g_vga[i] = vga_cell(' ', attr);
    }

    port_outb(CRTC_INDEX, CRTC_CURSOR_START);
    port_outb(CRTC_DATA, CURSOR_FIRST_LINE);
    port_outb(CRTC_INDEX, CRTC_CURSOR_END);
    port_outb(CRTC_DATA, CURSOR_LAST_LINE);
}

static void text_put(int row, int column, char character, k_u8_t attr)
{
    if (row < 0 || row >= VGA_ROWS || column < 0 || column >= VGA_COLUMNS) {
        return;
    }

    g_vga[row * VGA_COLUMNS + column] = vga_cell(character, attr);
}

/* The screen is one memory, so a scroll is a move of its rows - the words
 * carry their attributes with them - and a blank last one in the colour of
 * `attr`. The loop stops one row short of the end on purpose: the source of
 * the last move is the last row, which the second loop fills. */
static void text_scroll(k_u8_t attr)
{
    int i;

    for (i = 0; i < (int)VGA_CELLS - VGA_COLUMNS; i++) {
        g_vga[i] = g_vga[i + VGA_COLUMNS];
    }

    for (; i < (int)VGA_CELLS; i++) {
        g_vga[i] = vga_cell(' ', attr);
    }
}

static void text_cursor(int row, int column)
{
    k_u16_t position = (k_u16_t)(row * VGA_COLUMNS + column);

    port_outb(CRTC_INDEX, CRTC_CURSOR_LOW);
    port_outb(CRTC_DATA, (k_u8_t)(position & 0xFFu));
    port_outb(CRTC_INDEX, CRTC_CURSOR_HIGH);
    port_outb(CRTC_DATA, (k_u8_t)(position >> 8));
}

/*
 * The pair of `console` of /etc (ADR 0021) for every cell of the pair: the
 * theme and its inversion are the attribute the console computed, the
 * characters stay where they are, and a cell that carries a colour of its own
 * (a warning, a directory) keeps it. The line that was on the glass when a
 * person typed `config set console.theme light` is read again in the new
 * colours instead of disappearing.
 */
static void text_colors(const struct port_x86_64_pair *pair)
{
    k_u8_t next = PORT_X86_64_ATTR(pair->fg, pair->bg);
    k_u8_t old = g_theme_attr;
    int i;

    /* The line is written before the cells of the pair are repainted, so it
     * is one of them and the pass carries it to the new colours: a letter
     * written after the pass would be the pair of the screen already and
     * would read as the reverse of the old one when the two are the same two
     * colours traded (the dark and the light theme are exactly that). */
    kinfo("console: the attribute of the screen %d", (int)next);

    for (i = 0; i < (int)VGA_CELLS; i++) {
        k_u8_t attr = (k_u8_t)(g_vga[i] >> 8);

        if (attr == old) {
            attr = next;
        } else if (attr == attr_reverse(old)) {
            attr = attr_reverse(next);
        }

        g_vga[i] = (k_u16_t)((g_vga[i] & 0x00FFu) | ((k_u16_t)attr << 8));
    }

    g_theme_attr = next;
}

const struct port_x86_64_screen port_x86_64_screen_text = {
    .name = "the text mode of the VGA, 80x25",
    .columns = VGA_COLUMNS,
    .rows = VGA_ROWS,
    .clear = text_clear,
    .put = text_put,
    .scroll = text_scroll,
    .cursor = text_cursor,
    .colors = text_colors,
    /* The cells of the text mode are the picture: there is no first frame to
     * complete, and a redraw would write nothing the screen does not have. */
    .redraw = NULL,
};
