/*
 * Micro OS — the resources a program may ask about.
 *
 * The kernel counts the time of a task for its limit, but the ABI has no call
 * that hands the count to the task (the shell's `ulimit` reads the limit, not
 * the use). `getrusage` therefore answers `ENOSYS`: the timer of a command
 * still measures the wall time of the boot clock, and the CPU column stays
 * zero. The shape of the structure is here so that a portable source compiles
 * and asks (docs/debts.md).
 */
#ifndef MICRO_OS_LIBC_USER_SYS_RESOURCE_H
#define MICRO_OS_LIBC_USER_SYS_RESOURCE_H

#include "ulibc.h"

#include <sys/time.h>

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1

struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_ixrss;
    long ru_idrss;
    long ru_isrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nswap;
    long ru_inblock;
    long ru_oublock;
    long ru_msgsnd;
    long ru_msgrcv;
    long ru_nsignals;
    long ru_nvcsw;
    long ru_nivcsw;
};

int getrusage(int who, struct rusage *usage);

#endif /* MICRO_OS_LIBC_USER_SYS_RESOURCE_H */
