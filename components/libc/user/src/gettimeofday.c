/*
 * Micro OS — the clock of a program in the shape of `gettimeofday`.
 *
 * The moment is the wall clock of the call 49 (`KSYS_CLOCK_GETTIME`, the
 * CLOCK_REALTIME of POSIX), which is the date of the machine and not the
 * seconds since its boot: the timer of SQLite's shell and the statistics of
 * zstd measure the time that passed between two calls, and a command that
 * prints a date prints the date of the machine. A machine whose clock was
 * never set answers the seconds since the boot, which is a number that
 * orders the moments and not a day of the calendar.
 */
#include "ulibc.h"

#include "ulibc_internal.h"

#include <errno.h>
#include <sys/time.h>

int gettimeofday(struct timeval *moment, void *zone)
{
    struct ksys_timespec answer;
    long rc;

    (void)zone;

    if (moment == NULL) {
        errno = EINVAL;
        return -1;
    }

    rc = __ulibc_syscall6(KSYS_CLOCK_GETTIME, (long)KSYS_CLOCK_REALTIME, (long)(uintptr_t)&answer,
                          0, 0, 0, 0);
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }

    moment->tv_sec = (time_t)answer.tv_sec;
    moment->tv_usec = (long)(answer.tv_nsec / 1000);

    return 0;
}
