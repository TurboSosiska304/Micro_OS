/*
 * Micro OS — the console of the system.
 *
 * One place for the output of the characters: the serial port of the board
 * and, when it is registered, everything that asks for a copy — the display
 * of the board. Everything that prints something goes through here, so that
 * the output of the programs (through the libc) and the output of the shell
 * look the same. A task with a terminal of its own (a session of telnet)
 * writes to that terminal and not to the port: see `struct k_terminal`.
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
 * Register one more sink beside the others, for a reader of the console
 * that is not the display of the board. The sink that is registered once and
 * answers nothing while it has nothing to show is the intended use: the slot
 * it gets is not taken away behind its back.
 * Returns 0, or -1 when there is no empty slot left.
 */
int k_console_add_sink(k_console_sink_fn sink);

/*
 * Take a sink away. The slot it holds is free again, so a reader that goes
 * away does not leave a dead function behind - and a test does not leave one
 * in the system for every test that follows.
 */
void k_console_remove_sink(k_console_sink_fn sink);

/*
 * Print to the console: the serial port and every registered sink - or, for
 * a task that has a terminal of its own, to that terminal alone.
 */
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
 * A terminal of a task: the input and the output of a dialogue that is not
 * the console of the machine. A session of telnet is the case this exists
 * for, and the defect of 1.6 is the reason: `login` over the air read its
 * password from `k_console_getchar()`, which is the UART of the board, so a
 * remote person typed into a keyboard somewhere else.
 *
 * The terminal belongs to the task that set it. A task with none reads and
 * writes the console of the port, which is what every other task of the
 * system does, and what a session of the console itself does: the door of
 * the board needs no terminal of its own.
 *
 * The structure is copied by the session that runs on it, and `ctx` is the
 * state of the caller: a socket, a script of a test. `get` waits for the
 * byte (a terminal that is quiet is not a closed one) and answers -1 only
 * when it is gone; `put` writes the bytes of the server; `echo` says whether
 * the server repeats what the person types, which a client of telnet settles
 * by the option of echo (RFC 857) and the console always answers true.
 */
struct k_terminal {
    void *ctx;
    int (*get)(void *ctx);
    void (*put)(void *ctx, const char *data, k_size_t len);
    bool (*echo)(void *ctx);
};

/* The terminal of the calling task, or NULL for the console of the port. */
struct k_terminal *k_terminal_current(void);

/* Give the calling task a terminal, or take it away with NULL. */
void k_terminal_set(struct k_terminal *terminal);

/*
 * The input side of the console. The shell waits for a whole line,
 * /dev/tty0 asks whether a character is waiting and does not hold the
 * task that reads it, and the code that wants one key gets one.
 *
 * `k_console_getchar()` and `k_console_poll()` read the terminal of the
 * calling task when it has one and the console of the port otherwise. The
 * two calls below are the other half of a dialogue: the bytes a server
 * writes to its terminal, and the answer to "does this terminal want the
 * bytes echoed".
 */
int k_console_readline(char *buf, k_size_t size);
int k_console_getchar(void);
int k_console_poll(void);
void k_console_terminal_put(const char *data, k_size_t len);
bool k_console_terminal_echo(void);

/*
 * A character has arrived on the input of the console: the tasks that wait
 * for it in `poll` are woken. The port calls this where it puts the byte into
 * its ring - the interrupt of the keyboard, the poll of the serial port - and
 * a console whose input nobody watches costs one lock of the scheduler.
 */
void k_console_input_notify(void);

#endif /* MICRO_OS_KERNEL_CONSOLE_H */
