/*
 * Micro OS — the use of the resources of a program.
 *
 * The kernel counts the processor time of every task for the CPU limit and
 * keeps it in milliseconds (`struct task::cpu_ms`, kernel/task.h). The call 50
 * `KSYS_GETRUSAGE` hands that count to the task, so `getrusage` is the timer
 * of a profiler and the CPU column of a command and not a zero with success.
 *
 * This system makes no boundary between the user and the kernel on a machine
 * of one privilege level: a call of the board is a function call and the time
 * of the x86-64 kernel inside a call is the time of the caller. The whole of
 * the count is therefore reported as user time and the system time is zero,
 * which is a fact about the machine and not a field that was forgotten.
 *
 * The memory, the faults and the switches of POSIX are not counted by this
 * kernel: they are zero, and `docs/debts.md` names them. `RUSAGE_CHILDREN`
 * has no call - the usage of a child that has ended is not kept yet - so the
 * wrapper answers ENOSYS for it rather than a zero that looks like a number.
 */
#include "ulibc.h"

#include "ulibc_internal.h"

#include <errno.h>
#include <string.h>
#include <sys/resource.h>

int getrusage(int who, struct rusage *usage)
{
    struct ksys_rusage answer;
    long rc;

    if (usage == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(usage, 0, sizeof(*usage));

    if (who != RUSAGE_SELF) {
        errno = ENOSYS;
        return -1;
    }

    rc = __ulibc_syscall6(KSYS_GETRUSAGE, (long)(uintptr_t)&answer, 0, 0, 0, 0, 0);
    if (rc < 0) {
        errno = (rc == -ENOSYS) ? ENOSYS : (int)-rc;
        return -1;
    }

    usage->ru_utime.tv_sec = (time_t)(answer.utime_ms / 1000u);
    usage->ru_utime.tv_usec = (long)((answer.utime_ms % 1000u) * 1000u);
    usage->ru_stime.tv_sec = (time_t)(answer.stime_ms / 1000u);
    usage->ru_stime.tv_usec = (long)((answer.stime_ms % 1000u) * 1000u);

    return 0;
}

/* The selector of POSIX to the resource of the ABI, or -1 for a limit this
 * system does not have. */
static int limit_which(int resource)
{
    switch (resource) {
    case RLIMIT_CPU:
        return (int)KSYS_RLIMIT_CPU;
    case RLIMIT_NOFILE:
        return (int)KSYS_RLIMIT_FDS;
    case RLIMIT_AS:
        return (int)KSYS_RLIMIT_MEMORY;
    default:
        return -1;
    }
}

/*
 * The limits of the task in the shape of POSIX. The system keeps one value
 * and not a soft and a hard pair, so both fields of the answer are it, and a
 * `setrlimit` of a program is the one the kernel has: a raise is EPERM.
 */
int getrlimit(int resource, struct rlimit *limit)
{
    k_u64_t value = 0;
    long rc;
    int which = limit_which(resource);

    if (limit == NULL || which < 0) {
        errno = EINVAL;
        return -1;
    }

    rc = __ulibc_syscall6(KSYS_GETLIMIT, (long)which, (long)(uintptr_t)&value, 0, 0, 0, 0);
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }

    limit->rlim_cur = (rlim_t)value;
    limit->rlim_max = (rlim_t)value;

    return 0;
}

int setrlimit(int resource, const struct rlimit *limit)
{
    long rc;
    int which = limit_which(resource);

    if (limit == NULL || which < 0) {
        errno = EINVAL;
        return -1;
    }

    rc = __ulibc_syscall6(KSYS_SETLIMIT, (long)which, (long)limit->rlim_cur, 0, 0, 0, 0);
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }

    return 0;
}
