/*
 * Micro OS — the assertion of a program.
 *
 * A program of this system has no `abort` of the kernel to call: an assertion
 * that fails writes its sentence and ends the program with a status of 255,
 * which is the status the port gives a program that faulted.
 */
#ifndef MICRO_OS_LIBC_USER_ASSERT_H
#define MICRO_OS_LIBC_USER_ASSERT_H

void __ulibc_assert_fail(const char *expression, const char *file, unsigned line)
    __attribute__((noreturn));

#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression)                                                                         \
    ((expression) ? (void)0 : __ulibc_assert_fail(#expression, __FILE__, __LINE__))
#endif

#endif /* MICRO_OS_LIBC_USER_ASSERT_H */
