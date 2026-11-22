/*
 * Micro OS — the error number of a program.
 *
 * The number is the one the system call returned with its minus sign, kept in
 * the block of the program so that a ported library can read `errno` after a
 * call and print `strerror(errno)`. The system has one thread of a program and
 * no errno of a thread; the block is a plain global.
 */
#ifndef MICRO_OS_LIBC_USER_ERRNO_H
#define MICRO_OS_LIBC_USER_ERRNO_H

#include "ulibc.h"

#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define E2BIG 7
#define ENOEXEC 8
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EEXIST 17
#define EXDEV 18
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ENOSPC 28
#define ESPIPE 29
#define EPIPE 32
#define EDOM 33
#define ERANGE 34
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTEMPTY 39
#define ENOTSUP 95

#endif /* MICRO_OS_LIBC_USER_ERRNO_H */
