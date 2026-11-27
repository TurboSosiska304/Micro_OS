/*
 * Micro OS — test-only x86-64 clock seam.
 *
 * Production builds map wifi_test_ticks() to port_x86_64_ticks(). Host tests
 * define MICRO_OS_HOST_TEST and supply microos_test_x86_64_ticks(), so a probe
 * can be exercised without the PIT or a machine.
 */
#ifndef MICRO_OS_TEST_X86_64_TICKS_H
#define MICRO_OS_TEST_X86_64_TICKS_H

#include "kernel/types.h"

k_u64_t microos_test_x86_64_ticks(void);

static inline k_u64_t wifi_test_ticks(void)
{
    return microos_test_x86_64_ticks();
}

#endif /* MICRO_OS_TEST_X86_64_TICKS_H */
