/*
 * Micro OS — the use of the resources of a program.
 *
 * There is no call of the ABI that answers the time a task spent running; the
 * kernel counts it for the limit of the task and keeps it. The answer is
 * `ENOSYS` and not a zero with success, so a caller that prints a number
 * knows that the number is missing instead of reading a false zero. The timer
 * of SQLite's shell still has its wall clock, which is `gettimeofday`.
 */
#include "ulibc.h"

#include <errno.h>
#include <string.h>
#include <sys/resource.h>

int getrusage(int who, struct rusage *usage)
{
    (void)who;

    if (usage != NULL) {
        memset(usage, 0, sizeof(*usage));
    }

    errno = ENOSYS;
    return -1;
}
