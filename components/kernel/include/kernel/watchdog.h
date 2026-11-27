/*
 * Micro OS — the hang watchdog.
 *
 * A hang that has no name is found by accident, and the dump that finds it is
 * written for that one bug and turned off after. The watchdog is the opposite:
 * it runs always, it takes no lock, and when a task does not get CPU time or
 * does not progress for longer than the threshold it prints one dump of the
 * whole machine - once per incident, so the screen is not flooded.
 */
#ifndef MICRO_OS_KERNEL_WATCHDOG_H
#define MICRO_OS_KERNEL_WATCHDOG_H

#include "kernel/types.h"

#define K_WATCHDOG_THRESHOLD_MS 5000u

void k_watchdog_init(void);

/* Check for a hang. Safe to call from the scheduler tick (no locks). */
void k_watchdog_check(void);

/* Force a dump now (NMI, debug byte, gate timeout). No locks. */
void k_watchdog_trigger(const char *reason);

/* True after the first dump of the current incident. */
bool k_watchdog_triggered(void);

/* Clear the latch so the next incident is reported. */
void k_watchdog_reset(void);

#endif /* MICRO_OS_KERNEL_WATCHDOG_H */
