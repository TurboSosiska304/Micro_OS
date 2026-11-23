/*
 * Micro OS — the resources a program may ask about.
 *
 * The kernel counts the processor time of every task for its CPU limit, and
 * the call 50 (`KSYS_GETRUSAGE`) hands that count to the task: `ru_utime` is
 * the time the program spent running and `ru_stime` is zero, because this
 * system makes no boundary between the two on a machine of one privilege
 * level. `RUSAGE_CHILDREN` is `ENOSYS`: the usage of a child that has ended
 * is not kept yet (docs/debts.md). The memory, the faults and the switches of
 * POSIX are zero, and they are fields of the structure that this kernel does
 * not count.
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

/*
 * The limits of POSIX and the three the ABI has (kernel/ksys.h). The numbers
 * of the selectors are the ones of a Unix-like system, so a program that
 * knows one contract knows the other: the memory is RLIMIT_AS, the processor
 * time RLIMIT_CPU and the descriptors RLIMIT_NOFILE.
 *
 * The system keeps one value of each resource and not a soft and a hard one:
 * `rlim_cur` and `rlim_max` of the answer are the same number, and
 * `setrlimit` lowers the one the kernel has (a raise is EPERM).
 */
#define RLIMIT_CPU 0
#define RLIMIT_NOFILE 7
#define RLIMIT_AS 9

typedef unsigned long long rlim_t;

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

int getrlimit(int resource, struct rlimit *limit);
int setrlimit(int resource, const struct rlimit *limit);

#endif /* MICRO_OS_LIBC_USER_SYS_RESOURCE_H */
