/*
 * Micro OS — the console of the system.
 *
 * One place for the output of the characters: the serial port of the board
 * and, when it is registered, everything that asks for a copy — the
 * display of the board, the client of telnet. Everything that prints
 * something goes through here, so that the output of the programs
 * (through the libc) and the output of the shell look the same.
 */
#ifndef MICRO_OS_KERNEL_CONSOLE_H
#define MICRO_OS_KERNEL_CONSOLE_H

#include "kernel/types.h"

/* The sink that receives a copy of everything that is printed. */
typedef void (*k_console_sink_fn)(const char *data, k_size_t len);

/* The sinks that receive a copy: the display is the first of them. */
#define K_CONSOLE_SINK_MAX 3

/* Register the sink of the display. NULL removes it. */
void k_console_set_sink(k_console_sink_fn sink);

/*
 * Register one more sink beside the others — the client of telnet uses
 * this, so that a command over the air sees the same output as the
 * UART. The sink that is registered once and answers nothing while no
 * client is connected is the intended use: the slot it gets is not
 * taken away behind its back.
 * Returns 0, or -1 when there is no empty slot left.
 */
int k_console_add_sink(k_console_sink_fn sink);

/*
 * Take a sink away. The slot it holds is free again, so a client of telnet
 * that goes away does not leave a dead function behind - and a test does
 * not leave one in the system for every test that follows.
 */
void k_console_remove_sink(k_console_sink_fn sink);

/* Print to the console: the serial port and every registered sink. */
void k_console_write(const char *data, k_size_t len);

/*
 * Whether the console of this machine has colours: a screen that reads the
 * SGR sequences of a terminal. The port says so once, before the shell
 * exists; the default is off, because a serial port and the UART of the
 * board are plain text.
 *
 * The log asks this before it marks a line with the colour of its level
 * (kernel/log.h), so a plain console is not sent the bytes at all. The port
 * is free to keep its own copy plain as well: x86-64 strips the sequences
 * from COM1, which is the same line for a person without a window.
 */
void k_console_set_ansi(bool enabled);
bool k_console_ansi(void);

/*
 * The input side of the console. The shell waits for a whole line,
 * /dev/tty0 asks whether a character is waiting and does not hold the
 * task that reads it, and the code that wants one key gets one.
 */
int k_console_readline(char *buf, k_size_t size);
int k_console_getchar(void);
int k_console_poll(void);

/*
 * A character has arrived on the input of the console: the tasks that wait
 * for it in `poll` are woken. The port calls this where it puts the byte into
 * its ring - the interrupt of the keyboard, the poll of the serial port - and
 * a console whose input nobody watches costs one lock of the scheduler.
 */
void k_console_input_notify(void);

#endif /* MICRO_OS_KERNEL_CONSOLE_H */
