/*
 * Micro OS — the signals of a program.
 *
 * The four signals of the ABI (kernel/ksys.h): the interrupt of the console,
 * the broken pipe, the end of the task and the end of a child. `signal` is the
 * POSIX shape over the call of the kernel; SIG_ERR is `(void (*)(int))-1`, as
 * the standard says, and a successful answer is the old disposition.
 *
 * The header is the one of `feature/lua` (`components/libc/user/include/`)
 * with the functions in a file of their own.
 */
#ifndef MICRO_OS_LIBC_USER_SIGNAL_H
#define MICRO_OS_LIBC_USER_SIGNAL_H

#include "ulibc.h"

#define SIGINT 2
#define SIGPIPE 13
#define SIGTERM 15
#define SIGCHLD 17

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
