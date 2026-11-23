/*
 * Micro OS — the clock of a program.
 *
 * The kernel has one clock: `uptime`, the milliseconds since the boot. There
 * is no wall clock on the x86-64 machine and the filesystem keeps a moment of
 * Unix only when the tool that wrote the volume gave one. `time()` therefore
 * answers the seconds of the boot and not the epoch, and the tools that print
 * a date print the moment the record holds, as they would on a machine whose
 * clock had not been set. `docs/debts.md` records the missing clock.
 */
#ifndef MICRO_OS_LIBC_USER_TIME_H
#define MICRO_OS_LIBC_USER_TIME_H

#include <stddef.h>

#ifndef MICRO_OS_LIBC_USER_TIME_T_DEFINED
#define MICRO_OS_LIBC_USER_TIME_T_DEFINED
typedef long time_t;
#endif

#ifndef MICRO_OS_LIBC_USER_CLOCK_T_DEFINED
#define MICRO_OS_LIBC_USER_CLOCK_T_DEFINED
typedef long clock_t;
#endif

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

#define CLOCKS_PER_SEC 1000000L

/* The one clock of the machine is the monotonic one of the boot; a caller
 * that asks for the realtime clock gets the same moment, because there is no
 * second clock to give. */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

struct timespec;

int clock_gettime(int clock, struct timespec *moment);

time_t time(time_t *out);
clock_t clock(void);
/* The span of two moments. A program that computes in a double (`os.time` of
 * Lua) is built without -mgeneral-regs-only; `src/time.c` defines the call in
 * the build that sets ULIBC_WITH_FLOAT, which is the same one. */
double difftime(time_t later, time_t earlier);
struct tm *gmtime(const time_t *when);
struct tm *localtime(const time_t *when);
time_t mktime(struct tm *parts);
size_t strftime(char *buffer, size_t size, const char *format, const struct tm *parts);
char *asctime(const struct tm *parts);
char *ctime(const time_t *when);

#endif /* MICRO_OS_LIBC_USER_TIME_H */
