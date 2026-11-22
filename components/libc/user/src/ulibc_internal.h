/*
 * Micro OS — the seam between the files of the library of a program.
 *
 * The door of the system call is in `src/syscall.c`, and the two families of
 * headers that are not one call each — the information about an object
 * (`<sys/stat.h>`) and the entries of a directory (`<dirent.h>`) — are their
 * own files. Those files need the raw answer of the door and the POSIX shape
 * of it, and neither is a part of the interface a program includes: this
 * header is the seam, and it is not in `include/`.
 *
 * The shape of the answer is the one of the library (ulibc.h): a call answers
 * the negative number of the error, and a wrapper that POSIX puts around it
 * keeps the magnitude in `errno` and answers -1.
 */
#ifndef MICRO_OS_LIBC_USER_INTERNAL_H
#define MICRO_OS_LIBC_USER_INTERNAL_H

#include "ulibc.h"

/* The one door: the number of the call, six arguments, the raw answer. */
long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5);

/* The same answer in the POSIX shape: -1 (with errno) or the result. */
long __ulibc_result(long result);

#endif /* MICRO_OS_LIBC_USER_INTERNAL_H */
