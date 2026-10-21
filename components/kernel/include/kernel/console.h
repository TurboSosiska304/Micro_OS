/*
 * Micro OS — the console of the system.
 *
 * One place for the output of the characters: the serial port of the board
 * and, when it is registered, the display of the board. Everything that
 * prints something goes through here, so that the output of the programs
 * (through the libc) and the output of the shell look the same.
 */
#ifndef MICRO_OS_KERNEL_CONSOLE_H
#define MICRO_OS_KERNEL_CONSOLE_H

#include "kernel/types.h"

/* The sink that receives a copy of everything that is printed. */
typedef void (*k_console_sink_fn)(const char *data, k_size_t len);

/* Register the sink of the display. NULL removes it. */
void k_console_set_sink(k_console_sink_fn sink);

/* Print to the console: the serial port and the sink of the display. */
void k_console_write(const char *data, k_size_t len);

#endif /* MICRO_OS_KERNEL_CONSOLE_H */
