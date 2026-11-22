/*
 * Micro OS — the clock of a program.
 *
 * The kernel has no call that answers the seconds of Unix: the wall clock of
 * the system (`k_clock_now_unix`) is a fact of the kernel that the ABI of
 * 1.1 does not hand to a program. `time()` is therefore the time since the
 * boot, in seconds, and `os.date` of Lua shows the moment of the epoch plus
 * that count. When the ABI learns the wall clock, this file asks it and
 * nothing of the library of a program changes.
 *
 * The calendar is the C locale of UTC: no time zone, no daylight saving, and
 * the two functions `localtime` and `gmtime` answer the same structure.
 */
#ifndef MICRO_OS_LIBC_USER_TIME_H
#define MICRO_OS_LIBC_USER_TIME_H

#include "ulibc.h"

typedef long time_t;
typedef long clock_t;

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

time_t time(time_t *seconds);
clock_t clock(void);
double difftime(time_t later, time_t earlier);
struct tm *gmtime(const time_t *seconds);
struct tm *localtime(const time_t *seconds);
time_t mktime(struct tm *broken);
size_t strftime(char *buffer, size_t size, const char *format, const struct tm *broken);

#endif /* MICRO_OS_LIBC_USER_TIME_H */
