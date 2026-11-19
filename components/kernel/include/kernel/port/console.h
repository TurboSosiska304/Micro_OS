/*
 * Micro OS — platform port: the console.
 *
 * The bytes of the system go out here and the keys of the user come in
 * here. The kernel never opens a UART, a VFS or a terminal by itself: it
 * asks the port, and the port decides what "the console" means (the UART
 * of the board, the stdin of the host process).
 */
#ifndef MICRO_OS_KERNEL_PORT_CONSOLE_H
#define MICRO_OS_KERNEL_PORT_CONSOLE_H

#include "kernel/types.h"

/* Raw byte sink used by the log/console subsystem. */
void k_port_write(const char *data, k_size_t len);

/*
 * Read one line from the console. Returns the number of characters read
 * (without the newline) or -1 on end of input.
 */
int k_port_console_readline(char *buf, k_size_t size);

/*
 * Read one character from the console and wait for it. Returns the
 * character or -1 at the end of the input. The terminal of the user
 * repeats the character itself, the kernel does not have to.
 */
int k_port_console_getchar(void);

/*
 * Read one character if one is already waiting: the character, or -1 when
 * the console has nothing to give right now.
 *
 * The kernel needs both readings: the shell wants to wait for the user,
 * /dev/tty0 must not hold a task that reads it when the user is silent.
 */
int k_port_console_poll(void);

#endif /* MICRO_OS_KERNEL_PORT_CONSOLE_H */
