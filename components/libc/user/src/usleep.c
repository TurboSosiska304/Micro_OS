/*
 * Micro OS — a pause shorter than a second.
 *
 * `sleep` of POSIX counts seconds and the call of the ABI counts
 * milliseconds; SQLite's busy handler and the port layer of its VFS ask for a
 * pause of a few hundred microseconds and a second of granularity would turn
 * a retry into a stall. The call is rounded up to the next millisecond, the
 * smallest pause the kernel can make.
 */
#include "ulibc.h"

#include "ulibc_internal.h"

int usleep(unsigned microseconds)
{
    if (microseconds > 0) {
        long milliseconds = (long)((microseconds + 999u) / 1000u);

        (void)__ulibc_syscall6(KSYS_SLEEP, milliseconds, 0, 0, 0, 0, 0);
    }

    return 0;
}
