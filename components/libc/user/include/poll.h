/*
 * Micro OS — `poll`, in the shape of POSIX.
 *
 * The structure and the three bits are the ones of the ABI (kernel/ksys.h):
 * eight bytes of entry, a descriptor and two masks. The library does not
 * reinterpret them, so a program of this system and the kernel agree on the
 * bytes without a copy.
 */
#ifndef MICRO_OS_ULIBC_POLL_H
#define MICRO_OS_ULIBC_POLL_H

#include <stdint.h>

#include "kernel/ksys.h"

typedef unsigned long nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN KSYS_POLLIN
#define POLLOUT KSYS_POLLOUT
#define POLLERR KSYS_POLLERR

/* Wait for one of the descriptors: the count of the ready entries, 0 when the
 * timeout passed, -1 with `errno` when a signal cut the wait or the list is
 * not one this system accepts. A timeout of -1 waits without a deadline. */
int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms);

#endif /* MICRO_OS_ULIBC_POLL_H */
