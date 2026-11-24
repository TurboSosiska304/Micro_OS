/*
 * Micro OS — the signals of a program.
 *
 * The four signals of the ABI (kernel/ksys.h): the interrupt of the terminal,
 * the broken pipe, the end of the task and the end of a child. `signal` is the
 * POSIX shape over the call of the kernel, and `SIG_ERR` is `(void (*)(int))-1`
 * as the standard says; the kernel answers the old disposition with the same
 * value, so the two are told apart by the caller only when it asked for a
 * handler and got an error.
 *
 * A program that installs a handler of `SIGINT` gets it when the console sends
 * the interrupt; the library of Lua uses the disposition to end a long loop.
 */
#ifndef MICRO_OS_LIBC_USER_SIGNAL_H
#define MICRO_OS_LIBC_USER_SIGNAL_H

#include "ulibc.h"

#define SIGINT 2
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17
/*
 * The resize of a terminal, which this system does not have: the number is
 * here so that a source that asks for it compiles, and `signal()` answers
 * `SIG_ERR` for it (ADR 0051, the last section). An editor that would redraw
 * itself on a resize is told about nothing.
 */
#define SIGWINCH 28

typedef void (*sighandler_t)(int);

/* The type of a flag a signal handler may write: an int is written whole on
 * this machine, and that is the promise the standard makes of it. */
typedef int sig_atomic_t;

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t) - 1)

sighandler_t signal(int number, sighandler_t handler);
int raise(int number);

#endif /* MICRO_OS_LIBC_USER_SIGNAL_H */
