/*
 * Micro OS — COM1 and the last line of the screen for the boot loader.
 *
 * The port is programmed here and not assumed: the kernel programs it again
 * with its own call, and the loader's message is the only thing between the
 * reset and that call. The VGA row is the bottom one, so a refusal stays on
 * the screen even when the kernel never comes to clear it.
 */

#include "loader_io.h"

#define COM1 0x3F8u
#define VGA_TEXT 0xB8000u
#define VGA_COLUMNS 80u
#define VGA_ROW 24u
#define VGA_ATTRIBUTE 0x0Fu

static unsigned g_column_s;
static unsigned g_row_s = VGA_ROW;

static void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned char inb(unsigned short port)
{
    unsigned char value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));

    return value;
}

void loader_serial_init(void)
{
    outb(COM1 + 1, 0x00); /* no interrupt of the UART */
    outb(COM1 + 3, 0x80); /* the divisor latch is open */
    outb(COM1 + 0, 0x01); /* 115200 baud: 115200 / 1 */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* eight bits, no parity, one stop */
    outb(COM1 + 2, 0xC7); /* the FIFO, cleared */
    outb(COM1 + 4, 0x0B); /* DTR, RTS and OUT2 */
}

static void serial_put(char character)
{
    while ((inb(COM1 + 5) & 0x20u) == 0) {
        /* the transmitter holds a byte */
    }
    outb(COM1, (unsigned char)character);
}

static void vga_put(char character)
{
    volatile unsigned short *screen = (volatile unsigned short *)VGA_TEXT;
    unsigned cell;

    if (character == '\n') {
        return;
    }
    if (g_column_s >= VGA_COLUMNS) {
        return;
    }

    cell = g_row_s * VGA_COLUMNS + g_column_s;
    screen[cell] = (unsigned short)((unsigned char)character | (VGA_ATTRIBUTE << 8));
    g_column_s++;
}

void loader_puts(const char *text)
{
    while (*text != '\0') {
        if (*text == '\r') {
            serial_put('\r');
            g_column_s = 0;
        } else if (*text == '\n') {
            serial_put('\n');
            /* The loader has one line: the next one starts over it. */
            g_column_s = 0;
        } else {
            serial_put(*text);
            vga_put(*text);
        }
        text++;
    }
}
