/*
 * Micro OS — the fallback of a port that predates the cores of the kernel.
 *
 * kernel/port/cpu.h is the newest part of the contract: a port written before
 * it (the board in its compatibility mode, a port of a machine with one core)
 * does not carry the two calls, and the kernel must still link and run. The
 * weak definitions say "one core, and it is the one asking"; a port that
 * implements the call overrides them, and a port that reports more cores
 * implements both and raises its api_minor (ADR 0012).
 */
#include "kernel/port/cpu.h"

__attribute__((weak)) k_u32_t k_port_cpu_count(void)
{
    return 1;
}

__attribute__((weak)) k_u32_t k_port_cpu_id(void)
{
    return 0;
}
