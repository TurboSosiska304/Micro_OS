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

K_WEAK k_u32_t k_port_cpu_count(void)
{
    return 1;
}

K_WEAK k_u32_t k_port_cpu_id(void)
{
    return 0;
}

/*
 * A port with one core has nobody to wake, and a port with more of them that
 * does not implement the call is a port whose cores look at their queues on
 * their own ticks: the task waits for the next millisecond, which is what the
 * kernel did before the call existed (ADR 0012).
 */
K_WEAK void k_port_cpu_wake(k_u32_t cpu)
{
    K_UNUSED(cpu);
}
