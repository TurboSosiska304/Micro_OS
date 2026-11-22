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
 * The attribute of every cell is the `console` system of /etc (ADR 0021):
 * light grey on black, or black on light grey, with the inversion of the file
 * swapping the two.
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
 * like, so that the window and the log are the same picture. A system of /etc
 * (`console`, ADR 0021) may ask for the other attribute, and then every cell
 * of the screen carries it.
 *
 * The two attributes of the themes are grey on black and black on light grey.
 */
#define VGA_ATTR_DARK 0x07u
#define VGA_ATTR_LIGHT 0x70u

static k_u8_t g_attribute = VGA_ATTR_DARK;

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

static void text_clear(void)
{
    int i;

    for (i = 0; i < (int)VGA_CELLS; i++) {
        g_vga[i] = vga_cell(' ', g_attribute);
    }

    port_outb(CRTC_INDEX, CRTC_CURSOR_START);
    port_outb(CRTC_DATA, CURSOR_FIRST_LINE);
    port_outb(CRTC_INDEX, CRTC_CURSOR_END);
    port_outb(CRTC_DATA, CURSOR_LAST_LINE);
}

static void text_put(int row, int column, char character)
{
    if (row < 0 || row >= VGA_ROWS || column < 0 || column >= VGA_COLUMNS) {
        return;
    }

    g_vga[row * VGA_COLUMNS + column] = vga_cell(character, g_attribute);
}

/* The screen is one memory, so a scroll is a move of its rows and a blank last
 * one. The loop stops one row short of the end on purpose: the source of the
 * last move is the last row, which the second loop fills. */
static void text_scroll(void)
{
    int i;

    for (i = 0; i < (int)VGA_CELLS - VGA_COLUMNS; i++) {
        g_vga[i] = g_vga[i + VGA_COLUMNS];
    }

    for (; i < (int)VGA_CELLS; i++) {
        g_vga[i] = vga_cell(' ', g_attribute);
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
 * The attribute of the screen for every cell: the theme of `console` of /etc
 * (ADR 0021). The characters stay where they are, so the line that was on the
 * glass when a person typed `config set console.theme light` is read again in
 * the new colours instead of disappearing.
 */
static void text_colors(bool light, bool invert)
{
    k_u8_t attribute = light ? VGA_ATTR_LIGHT : VGA_ATTR_DARK;
    int i;

    if (invert) {
        attribute = (k_u8_t)((k_u8_t)(attribute >> 4) | (k_u8_t)(attribute << 4));
    }

    g_attribute = attribute;

    for (i = 0; i < (int)VGA_CELLS; i++) {
        g_vga[i] = (k_u16_t)((g_vga[i] & 0x00FFu) | ((k_u16_t)attribute << 8));
    }

    kinfo("console: the attribute of the screen %d", (int)attribute);
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
