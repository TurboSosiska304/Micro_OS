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
 * which: it counts cells, keeps the cursor, scrolls and reads the two
 * sequences of `clear`, and the screen it was given puts a cell where it says.
 * src/console.c is that half; src/vga.c and src/fb.c are the two screens.
 *
 * Nothing here is part of the contract of the port (kernel/port/console.h):
 * these are the numbers and the calls of one machine, and the kernel knows
 * neither the 0xB8000 nor the line of the keyboard.
 */
#ifndef MICRO_OS_PORT_X86_64_CONSOLE_H
#define MICRO_OS_PORT_X86_64_CONSOLE_H

#include "kernel/types.h"

/*
 * One screen of the machine as the console needs it: a grid of cells of its
 * own size, three operations that change it, the cursor and the colours of
 * `console` of /etc (ADR 0021).
 *
 * The console holds the position and the state of the escape sequence; the
 * screen holds the cells. A screen that draws directly (the text mode) writes
 * the cell where it is told; the framebuffer keeps a grid of characters and
 * draws the one cell that changed, so a line of text is a line of cells and
 * not a repaint of the picture.
 */
struct port_x86_64_screen {
    const char *name;

    /* The room of the dialogue in cells. */
    int columns;
    int rows;

    /* Blank the whole screen; the console puts the cursor at 0, 0 next. */
    void (*clear)(void);

    /* One character in one cell. */
    void (*put)(int row, int column, char character);

    /*
     * The picture moves up by one row and the last row is blank: the console
     * counts the rows and the screen knows how its memory is turned.
     */
    void (*scroll)(void);

    /*
     * The cursor is at (row, column), after the move. The text screen puts
     * the position in the CRTC and the framebuffer draws the cell in the
     * colours of the cursor.
     */
    void (*cursor)(int row, int column);

    /*
     * The pair of `console`: `light` is the theme (black on light grey) and
     * `invert` swaps the two. The screen repaints what is already there, so
     * the line a person typed is read again in the new colours.
     */
    void (*colors)(bool light, bool invert);
};

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
 * the shell, ESC[2J and ESC[H - is understood; the other sequences of a
 * terminal are dropped whole rather than printed.
 */
void port_x86_64_console_write(const char *data, k_size_t len);

/* Register the `console` system of /etc with the machine (ADR 0021). */
int port_x86_64_console_settings_register(void);

/*
 * Bring the controller of the keyboard up and let its interrupt through the
 * controller: the first port on, its clock running, the line of IRQ1 armed.
 * The mask of the 8259 is the business of pic_init(), which runs after this
 * one and opens the line.
 */
void port_x86_64_ps2_init(void);

/* The body of IRQ1: one byte of the keyboard, then the EOI. Called from
 * interrupts.S with the interrupts off. */
void port_x86_64_irq_keyboard(k_u64_t *frame);

/* The next character of the keyboard, or -1 when the ring is empty. */
int port_x86_64_ps2_poll(void);

#endif /* MICRO_OS_PORT_X86_64_CONSOLE_H */
