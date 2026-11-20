/*
 * Micro OS — the console of the x86-64 port: a text screen and a keyboard.
 *
 * The console of the machine is two devices that a person reads as one: the
 * 80x25 text mode of the VGA, where the kernel draws the same bytes it sends
 * to COM1, and the PS/2 keyboard, which answers with characters through an
 * interrupt. port_x86_64.c puts the halves together: k_port_write() writes
 * to both sinks and k_port_console_poll()/getchar() take a key from the ring
 * of the keyboard or a byte from the UART, in that order.
 *
 * Nothing here is part of the contract of the port (kernel/port/console.h):
 * these are the numbers and the calls of one machine, and the kernel knows
 * neither the 0xB8000 nor the line of the keyboard.
 */
#ifndef MICRO_OS_PORT_X86_64_CONSOLE_H
#define MICRO_OS_PORT_X86_64_CONSOLE_H

#include "kernel/types.h"

/*
 * Take the text screen over: clear it, put the cursor at the top left and
 * put that position in the CRTC, so that the cursor of the hardware is where
 * the next character will land.
 */
void port_x86_64_vga_init(void);

/*
 * Draw the bytes on the text screen. The control characters are the ones a
 * terminal has: '\n' ends the line (and scrolls when there is no more
 * screen), '\r' returns to its start, '\t' goes to the next stop of eight,
 * '\b' steps back and erases. Everything below 0x20 that is not one of those
 * is dropped, because a text cell holds a character and not a command.
 *
 * The one escape sequence the kernel writes - the "clear the screen" pair of
 * the shell, ESC[2J and ESC[H - is understood; the other sequences of a
 * terminal are dropped whole rather than printed.
 */
void port_x86_64_vga_write(const char *data, k_size_t len);

/*
 * Bring the controller of the keyboard up and let its interrupt through the
 * controller: the first port on, its clock running, the line of IRQ1 armed.
 * The mask of the 8259 is the business of pic_init(), which runs after this
 * one and opens the line.
 */
void port_x86_64_ps2_init(void);

/* The body of IRQ1: one byte of the keyboard, then the EOI. Called from
 * interrupts.S with the interrupts off. */
void port_x86_64_irq_keyboard(void);

/* The next character of the keyboard, or -1 when the ring is empty. */
int port_x86_64_ps2_poll(void);

#endif /* MICRO_OS_PORT_X86_64_CONSOLE_H */
