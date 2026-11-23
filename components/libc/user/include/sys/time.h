/*
 * Micro OS — the moment of a file and the timer of a program.
 *
 * `gettimeofday` is the wall clock of the machine (the call 49, the same one
 * `time()` asks): a timer that needs two moments of one kind gets them from
 * it, and a command that prints a date prints the date of the machine. The
 * moment of a file comes from the VFS through `stat`.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_TIME_H
#define MICRO_OS_LIBC_USER_SYS_TIME_H

#include "ulibc.h"

#ifndef MICRO_OS_LIBC_USER_TIME_T_DEFINED
#define MICRO_OS_LIBC_USER_TIME_T_DEFINED
typedef long time_t;
#endif

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *moment, void *zone);
int utimes(const char *path, const struct timeval times[2]);

#endif /* MICRO_OS_LIBC_USER_SYS_TIME_H */
