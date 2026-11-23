/*
 * Micro OS — the end of a child, for a portable source.
 *
 * `waitpid` answers the pid of the child that ended and writes the status the
 * kernel keeps; these macros read that status the POSIX way. The status of a
 * program of this system is the low byte of the code it exited with, which is
 * the number `WEXITSTATUS` reads.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_WAIT_H
#define MICRO_OS_LIBC_USER_SYS_WAIT_H

#include "../ulibc.h"
#include "types.h"

#define WNOHANG 1

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WIFSIGNALED(status) ((((status) & 0x7f) + 1) >= 2)
#define WTERMSIG(status) ((status) & 0x7f)
#define WIFSTOPPED(status) 0
#define WSTOPSIG(status) 0

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* MICRO_OS_LIBC_USER_SYS_WAIT_H */
