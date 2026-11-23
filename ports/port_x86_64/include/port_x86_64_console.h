/*
 * Micro OS — the console of the x86-64 port: a screen and a keyboard.
 *
 * The console of the machine is two devices that a person reads as one: a
 * screen, where the kernel draws the same bytes it sends to COM1, and the
 * PS/2 keyboard, which answers with characters through an interrupt.
 * port_x86_64.c puts the halves together: k_port_write() writes to both sinks
 * and k_port_console_poll()/getchar() take a key from the ring of the
 * keyboard or a byte from the UART, in that order.
 *
 * The screen is a framebuffer (ADR 0023) or the text mode of the VGA when the
 * machine has no interface to program one, and the console does not know
 * which: it counts cells, keeps the cursor, scrolls, reads the two sequences
 * of `clear` and the SGR sequences that choose the colours of the text, and
 * the screen it was given puts a cell where it says. src/console.c is that
 * half; src/vga.c and src/fb.c are the two screens.
 *
 * Nothing here is part of the contract of the port (kernel/port/console.h):
 * these are the numbers and the calls of one machine, and the kernel knows
 * neither the 0xB8000 nor the line of the keyboard.
 */
#ifndef MICRO_OS_PORT_X86_64_CONSOLE_H
#define MICRO_OS_PORT_X86_64_CONSOLE_H

#include "kernel/types.h"

/*
 * One attribute of a cell: the paper in the high nibble and the ink in the
 * low one, the pair a VGA calls (background << 4) | foreground, named by the
 * sixteen colours of the CGA palette (src/fb.c, port_x86_64_palette). The
 * console computes it from the SGR state of the dialogue (kernel/ansi.h) and
 * the screen stores it with the character, so a scroll, a repaint and a
 * change of the theme keep the colour of every word.
 */
#define PORT_X86_64_ATTR(fg, bg) ((k_u8_t)((((bg) & 0x0F) << 4) | ((fg) & 0x0F)))
#define PORT_X86_64_ATTR_FG(attr) ((k_u8_t)((attr) & 0x0F))
#define PORT_X86_64_ATTR_BG(attr) ((k_u8_t)(((attr) >> 4) & 0x0F))

/* The pair of the console: the ink and the paper of the plain text. `ink` and
 * `paper` are the numbers the framebuffer writes, so a person may name a
 * colour of 0xRRRR and not only one of the sixteen; `fg` and `bg` are the
 * nearest of the sixteen, which is the whole of what the text mode of the VGA
 * can carry. */
struct port_x86_64_pair {
    k_u16_t ink;
    k_u16_t paper;
    k_u8_t fg;
    k_u8_t bg;
};

/*
 * One screen of the machine as the console needs it: a grid of cells of its
 * own size, the operations that change it, the cursor and the colours of
 * `console` of /etc (ADR 0021).
 *
 * The console holds the position, the state of the escape sequences and the
 * attribute of the text; the screen holds the cells and their attributes. A
 * screen that draws directly (the text mode) writes the cell where it is
 * told; the framebuffer keeps a grid and draws the one cell that changed, so
 * a line of text is a line of cells and not a repaint of the picture.
 */
struct port_x86_64_screen {
    const char *name;

    /* The room of the dialogue in cells. */
    int columns;
    int rows;

    /*
     * Blank the whole screen and make every cell a space with `attr`; the
     * console puts the cursor at 0, 0 next.
     */
    void (*clear)(k_u8_t attr);

    /* One character in one cell, in the colour of its attribute. */
    void (*put)(int row, int column, char character, k_u8_t attr);

    /*
     * The picture moves up by one row and the last row is blank cells of
     * `attr`: the console counts the rows and the screen knows how its memory
     * is turned. The attributes of the rows that stay move with them.
     */
    void (*scroll)(k_u8_t attr);

    /*
     * The cursor is at (row, column), after the move. The text screen puts
     * the position in the CRTC and the framebuffer draws the cell in the
     * reverse of its own colours.
     */
    void (*cursor)(int row, int column);

    /*
     * The pair of `console`: the ink and the paper of the plain text, which
     * the theme of the file chose. The screen repaints what is already there,
     * so the line a person typed is read again in the new colours; a cell
     * that carries a colour of its own - a warning, a directory - keeps it.
     */
    void (*colors)(const struct port_x86_64_pair *pair);

    /*
     * Draw the whole picture again from what the screen holds. A screen that
     * *is* the memory (the text mode) has nothing to do and leaves it NULL;
     * the framebuffer uses it to put every byte of the picture through the
     * display once more, which is how the first frame of the boot is made
     * whole. See port_x86_64_console_redraw().
     */
    void (*redraw)(void);
};

/*
 * The sixteen colours of the CGA/VGA palette in RGB565, indexed by the nibble
 * of an attribute: black, blue, green, cyan, red, magenta, brown (amber),
 * light grey, then the bright eight. The text mode names them and the
 * framebuffer paints them, so the two screens of the port agree on every
 * colour a command asks for (src/fb.c).
 */
extern const k_u16_t port_x86_64_palette[16];

/* The index of the sixteen colours nearest to an RGB565 number: a person may
 * name any colour of 0xRRRR and the text mode has sixteen. */
k_u8_t port_x86_64_color_nearest(k_u16_t rgb);

/* The text mode of the VGA, 80x25 (src/vga.c): the fallback of ADR 0023. */
extern const struct port_x86_64_screen port_x86_64_screen_text;

/*
 * Choose the screen of the machine and clear it: the framebuffer when the
 * Bochs VBE interface answers (src/vbe.c, src/fb.c), the text mode otherwise.
 * Called by port_x86_64_init() once the page tables can map the window of the
 * card.
 */
void port_x86_64_console_init(void);

/* The name of the screen that was chosen, "text" before the boot chose one. */
const char *port_x86_64_console_name(void);

/*
 * Draw the bytes on the screen: one call, the same bytes that go to COM1.
 *
 * The control characters are the ones a terminal has: '\n' ends the line (and
 * scrolls when there is no more screen), '\r' returns to its start, '\t' goes
 * to the next stop of eight, '\b' steps back and erases. Everything below
 * 0x20 that is not one of those is dropped, because a cell holds a character
 * and not a command.
 *
 * The one escape sequence the kernel writes - the "clear the screen" pair of
 * the shell, ESC[2J and ESC[H - and the SGR sequences of the colours are
 * understood (kernel/ansi.h); the other sequences of a terminal are dropped
 * whole rather than printed. COM1 gets the same bytes without the SGR
 * sequences, so the log of a machine with a coloured screen is still a plain
 * transcript (src/port_x86_64.c).
 */
void port_x86_64_console_write(const char *data, k_size_t len);

/*
 * Draw the whole picture again: the cells the console holds are redrawn into
 * the screen, so nothing of the log is lost. The boot calls it once, after
 * the first lines of the boot are on the screen: the switch from the text
 * mode of the firmware to the picture of the port is a moment when a display
 * may still hold what was drawn before it, and a second pass over every pixel
 * - with the log already in it - is what leaves no line of the old screen
 * underneath the new one.
 */
void port_x86_64_console_redraw(void);

/* Register the `console` system of /etc with the machine (ADR 0021). */
int port_x86_64_console_settings_register(void);

/*
 * Bring the controller of the keyboard up and let its interrupt through the
 * controller: the first port on, its clock running, the line of IRQ1 armed,
 * and the scancode set of the keyboard asked (0xF0 0x00) before a byte of it
 * is read as a key. The mask of the 8259 is the business of pic_init(), which
 * runs after this one and opens the line.
 */
void port_x86_64_ps2_init(void);

/*
 * Which scancode set the keyboard of this machine speaks: the set the bytes
 * the handler reads are in (1, 2 or 3 - the set 1 when the controller of the
 * motherboard translates them, which is the machine of QEMU and of almost
 * every PC), and the set the device answered when it was asked (0 when it did
 * not answer: an XT, a controller that does not pass the command, or a
 * machine whose firmware took the keyboard away). The boot prints both, and
 * they differ on a laptop of the set 2 whose firmware leaves the translation
 * off - the machine of 1.8, where the set is the difference between a
 * keyboard that types and one that does not.
 */
k_u8_t port_x86_64_ps2_set(void);
k_u8_t port_x86_64_ps2_device_set(void);

/* The body of IRQ1: one byte of the keyboard, then the EOI. Called from
 * interrupts.S with the interrupts off. */
void port_x86_64_irq_keyboard(k_u64_t *frame);

/* The next character of the keyboard, or -1 when the ring is empty. */
int port_x86_64_ps2_poll(void);

#endif /* MICRO_OS_PORT_X86_64_CONSOLE_H */
