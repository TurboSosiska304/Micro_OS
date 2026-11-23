/*
 * Micro OS — the clock of a program.
 *
 * The machine has two clocks and the kernel answers both through the call 49
 * (`clock_gettime`): CLOCK_REALTIME is the date of Unix, which the port seeds
 * at the boot from the clock the machine owns and `ntp` corrects afterwards,
 * and CLOCK_MONOTONIC is the milliseconds since the boot, which is what
 * `uptime` and `clock()` of a moment are. A machine whose clock was never set
 * answers the seconds since its boot, and a caller that prints the number as
 * a date asks `gettimeofday` or `stat` of a file for a real one.
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

/* `clock()` counts the milliseconds of the processor time of the program, as
 * POSIX says; the moment of the boot is `clock_gettime(CLOCK_MONOTONIC)`. */
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

/* The two clocks of the machine: the date of the machine, which the port seeds
 * at the boot and `ntp` corrects, and the count of the boot. */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

#if !defined(MICRO_OS_LIBC_USER_TIMESPEC_DEFINED) && !defined(_STRUCT_TIMESPEC)
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
/* The fixed form of a moment, "Www Mmm dd hh:mm:ss yyyy\n": one string of 26
 * bytes that the next call overwrites, as the standard says. `file` names the
 * two for the date of a CDF record. */
char *asctime(const struct tm *parts);
char *ctime(const time_t *when);

#endif /* MICRO_OS_LIBC_USER_TIME_H */
