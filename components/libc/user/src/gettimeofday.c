/*
 * Micro OS — the clock of a program in the shape of `gettimeofday`.
 *
 * The one clock of the machine is the milliseconds since the boot; a caller
 * that needs two moments (the timer of SQLite's shell) gets them from the
 * same clock, so the difference is the time that really passed. There is no
 * wall clock, and the seconds of the answer are the seconds of the boot
 * (`docs/debts.md`).
 */
#include "ulibc.h"

#include <errno.h>
#include <sys/time.h>

int gettimeofday(struct timeval *moment, void *zone)
{
    long milliseconds;

    (void)zone;

    if (moment == NULL) {
        errno = EINVAL;
        return -1;
    }

    milliseconds = uptime();
    moment->tv_sec = (time_t)(milliseconds / 1000);
    moment->tv_usec = (long)((milliseconds % 1000) * 1000);

    return 0;
}
