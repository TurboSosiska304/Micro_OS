/*
 * Micro OS — the disposition of a signal in the shape POSIX gives it.
 *
 * The functions are the two wrappers of `feature/lua`'s `src/syscall.c` in a
 * file of their own; the table of the handlers, the frame of the ring 3 and
 * the restorer are the kernel's (`sys_signal`, `sys_sigreturn`). `raise` is
 * the call of `kill` of the own pid.
 */
#include "ulibc.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "ulibc_internal.h"

sighandler_t signal(int number, sighandler_t handler)
{
    long rc = __ulibc_syscall6(KSYS_SIGNAL, number, (long)(uintptr_t)handler, 0, 0, 0, 0);

    if (rc < 0) {
        errno = (int)-rc;
        return SIG_ERR;
    }

    return (sighandler_t)(uintptr_t)rc;
}

int raise(int number)
{
    long rc = __ulibc_syscall6(KSYS_KILL, getpid(), number, 0, 0, 0, 0);

    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }

    return 0;
}
