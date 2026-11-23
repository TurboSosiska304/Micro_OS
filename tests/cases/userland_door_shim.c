/*
 * Micro OS — the door of the program for the tests of the library on the host.
 *
 * The pure part of the library of a program is compiled for the host and runs
 * as an ordinary process: there is no kernel behind it, no window of a
 * program and no vector 128. The functions of the library that ask the
 * kernel still have to link, and this file is the door they find: every call
 * answers zero, which is the "success with the number zero" of the calls this
 * test exercises (`getuid`, `geteuid`, `getgid`, `setuid`, `setgid`).
 *
 * The real door is `components/libc/user/src/syscall.c`, and its crossing is
 * covered by `make x86-smoke` where a program of the machine calls the kernel.
 */
#include <errno.h>

/* The seam of the library (`src/ulibc_internal.h`), declared here because the
 * header of the seam is private to the directory of the library. */
long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5);
long __ulibc_result(long result);

long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5)
{
    (void)number;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    (void)a4;
    (void)a5;

    return 0;
}

long __ulibc_result(long result)
{
    if (result < 0) {
        errno = (int)-result;
    }

    return result;
}
