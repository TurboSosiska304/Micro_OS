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

/* Print to the console: the serial port and every registered sink. */
void k_console_write(const char *data, k_size_t len);

#endif /* MICRO_OS_KERNEL_CONSOLE_H */
