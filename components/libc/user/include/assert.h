/*
 * Micro OS — the assertion of a program.
 *
 * A failed assertion prints the expression, the file and the line on the
 * console of the errors and ends the program with the status of `abort` of
 * the C library. There is no backtrace to print: the image has no symbols of
 * a debugger in it.
 */
#ifndef MICRO_OS_LIBC_USER_ASSERT_H
#define MICRO_OS_LIBC_USER_ASSERT_H

#include "ulibc.h"

void __assert_fail(const char *expression, const char *file, unsigned line, const char *function)
    __attribute__((noreturn));

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression)                                                                         \
    ((expression) ? (void)0 : __assert_fail(#expression, __FILE__, __LINE__, __func__))
#endif

#endif /* MICRO_OS_LIBC_USER_ASSERT_H */
