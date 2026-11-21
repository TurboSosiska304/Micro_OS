/*
 * Micro OS — the wall clock of the system.
 *
 * The date is kept by a chip (the BM8563 on the board), and a chip is a
 * driver. But the time is not a private matter of the driver: a service
 * may set it from the network, and later the filesystem will stamp a file
 * with it. So the driver registers the clock here, and everybody else
 * gives and asks for the seconds of Unix.
 *
 * This is how Linux keeps the time of the wall clock in the kernel and
 * synchronises the RTC with it (hwclock), rather than letting every
 * program talk to the chip or format a command for a shell.
 */
#ifndef MICRO_OS_KERNEL_CLOCK_H
#define MICRO_OS_KERNEL_CLOCK_H

#include "kernel/types.h"

/*
 * Set the wall clock, in the seconds of Unix (UTC). Returns 0 on success,
 * or -1 when the board has no clock or the chip refuses the moment.
 */
typedef int (*k_clock_set_fn)(k_u64_t unix_seconds);

/*
 * Install the clock of the platform. The driver calls it once, when it
 * comes up; NULL takes the clock away (the host build and the tests have
 * none).
 */
void k_clock_register(k_clock_set_fn set);

/* Set the clock. -1 when there is no clock to set. */
int k_clock_set_unix(k_u64_t unix_seconds);

/*
 * The moment of the wall clock, in the seconds of Unix (UTC), for the
 * filesystem and for anybody who needs a stamp.
 *
 * A clock that was never set - a host build with no RTC, an x86-64 machine
 * with no battery - still gives a number: the seconds since the boot. It is
 * not a date, but it orders the changes of two records, and zero is kept
 * for "this filesystem knows no time" rather than for a machine without an
 * RTC. A caller that prints the number as a date should ask
 * k_clock_present() first.
 */
k_u64_t k_clock_now_unix(void);

/* Is there a clock at all? */
bool k_clock_present(void);

#endif /* MICRO_OS_KERNEL_CLOCK_H */
