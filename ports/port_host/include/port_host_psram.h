/*
 * Micro OS — the PSRAM of the host port.
 *
 * A separate pool of memory, so that the tests can check where the memory
 * of the different priorities comes from.
 */
#ifndef MICRO_OS_PORT_HOST_PSRAM_H
#define MICRO_OS_PORT_HOST_PSRAM_H

#include <stdbool.h>

#include "kernel/types.h"

void host_psram_init(void);
void *host_psram_alloc(k_size_t size);
void host_psram_release(void *ptr);
k_size_t host_psram_total(void);
k_size_t host_psram_available(void);
bool host_psram_contains(const void *ptr);

#endif /* MICRO_OS_PORT_HOST_PSRAM_H */
