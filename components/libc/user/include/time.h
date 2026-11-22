/*
 * Micro OS — the clock of a program.
 *
 * The kernel has one clock: `uptime`, the milliseconds since the boot. There
 * is no wall clock on the x86-64 machine and the filesystem keeps a moment of
 * Unix only when the tool that wrote the volume gave one. `time()` therefore
 * answers the seconds of the boot and not the epoch, and the tools that print
 * a date print the moment the record holds, as they would on a machine whose
 * clock had not been set. `docs/debts.md` records the missing clock.
 *
 * The calendar is the C locale of UTC: no time zone, no daylight saving, and
 * the two functions `localtime` and `gmtime` answer the same structure.
 */
#ifndef MICRO_OS_LIBC_USER_TIME_H
#define MICRO_OS_LIBC_USER_TIME_H

#include "ulibc.h"

#ifndef MICRO_OS_LIBC_USER_TIME_T_DEFINED
#define MICRO_OS_LIBC_USER_TIME_T_DEFINED
typedef long time_t;
#endif

#ifndef MICRO_OS_LIBC_USER_CLOCK_T_DEFINED
#define MICRO_OS_LIBC_USER_CLOCK_T_DEFINED
typedef long clock_t;
#endif

/* `clock()` counts the milliseconds of the boot, as `uptime` does. */
#define CLOCKS_PER_SEC 1000

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

/* The one clock of the machine is the monotonic one of the boot; a caller
 * that asks for the realtime clock gets the same moment, because there is no
 * second clock to give. */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

#ifndef MICRO_OS_LIBC_USER_TIMESPEC_DEFINED
#define MICRO_OS_LIBC_USER_TIMESPEC_DEFINED
struct timespec {
    time_t tv_sec;
    long tv_nsec;
};
#endif

int clock_gettime(int clock, struct timespec *moment);

time_t time(time_t *out);
clock_t clock(void);
double difftime(time_t later, time_t earlier);
struct tm *gmtime(const time_t *when);
struct tm *localtime(const time_t *when);
time_t mktime(struct tm *parts);
size_t strftime(char *buffer, size_t size, const char *format, const struct tm *parts);

#endif /* MICRO_OS_LIBC_USER_TIME_H */
