/*
 * Micro OS — the end of a child, for a portable source.
 *
 * `waitpid` answers the pid of the child that ended and writes the status the
 * kernel keeps. The status of a program of this system is the code it exited
 * with, 0 to 255, and the end of a task by a signal is the code 128 + the
 * number of the signal (the convention of architecture.md and of the shell).
 * It is not the 16-bit word of POSIX, where the exit code and the signal live
 * in two fields, so the macros below are the reading of this status and not
 * the masking of the other one.
 *
 * The two ranges do not overlap and the convention is the one the kernel
 * writes: 0 to 127 is a program that returned, 128 to 255 is a program that
 * was taken down by the signal `status - 128`. A program that calls
 * `exit(200)` is reported as the signal 72, which is the price of a status
 * that is one byte wide; a source of a Unix-like system rarely chooses a code
 * above 127, and the shell of the machine prints the number as it is.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_WAIT_H
#define MICRO_OS_LIBC_USER_SYS_WAIT_H

#include "../ulibc.h"
#include "types.h"

#define WNOHANG 1

#define WIFEXITED(status) ((status) >= 0 && (status) < 128)
#define WEXITSTATUS(status) ((status) & 0xff)
#define WIFSIGNALED(status) ((status) >= 128)
#define WTERMSIG(status) ((status) - 128)
#define WIFSTOPPED(status) 0
#define WSTOPSIG(status) 0

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);

#endif /* MICRO_OS_LIBC_USER_SYS_WAIT_H */
