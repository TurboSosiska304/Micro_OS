/*
 * Micro OS — host platform port (Linux simulator / unit test harness).
 */
#ifndef MICRO_OS_PORT_HOST_H
#define MICRO_OS_PORT_HOST_H

#include <stdio.h>

#include "kernel/types.h"

/*
 * Redirect all kernel output to the given stream (NULL restores stdout).
 * Used by the unit tests to capture log output.
 */
void port_host_set_sink(FILE *sink);
FILE *port_host_get_sink(void);

/*
 * Start the 1 ms time base: a background thread calls k_sched_tick() every
 * interval_ms milliseconds.
 */
void port_host_start_tick(long interval_ms);
void port_host_stop_tick(void);

#endif /* MICRO_OS_PORT_HOST_H */
