/*
 * Micro OS — the terminal of a program is not a terminal.
 *
 * The ABI has an `ioctl` and it answers `EINVAL` for every request
 * (`docs/debts.md`): there is no mode, no window size and no key without a
 * line. A portable source still includes this header for the shape of a
 * request, so the header names no request at all: a caller that asks whether
 * `TIOCGWINSZ` is there finds it missing and uses its own default width,
 * which is what a command of this system does.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_IOCTL_H
#define MICRO_OS_LIBC_USER_SYS_IOCTL_H

#include "ulibc.h"

#endif /* MICRO_OS_LIBC_USER_SYS_IOCTL_H */
