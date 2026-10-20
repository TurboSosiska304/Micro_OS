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

#endif /* MICRO_OS_PORT_HOST_H */
