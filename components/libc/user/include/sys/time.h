/*
 * Micro OS — the moment of a file and the timer of a program.
 *
 * The kernel has one clock, `uptime`, and the machine has no wall clock
 * (`docs/debts.md`): `gettimeofday` answers the moment of the boot in the
 * shape a portable source expects, because the timer of a command needs two
 * moments of one kind and not a date. The date of a file comes from the VFS
 * through `stat`, not from here.
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
