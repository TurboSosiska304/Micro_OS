/*
 * Micro OS — platform port: the cores.
 *
 * The kernel keeps the scheduler of every core (kernel/cpu.h); the port is
 * the only side that knows how many cores the machine has, which one is
 * running a call, and whether they were started at all. Linux splits the same
 * question between smp_processor_id() (the architecture) and the CPU masks of
 * the generic code, and this header is the architecture half.
 *
 * A port that says nothing is one core: the two calls have a weak default in
 * the portable kernel (components/kernel/src/cpu_default.c), so a port
 * written before this part of the contract still links and runs on one core
 * with the same meaning. A port that speaks the new contract implements both
 * and raises its api_minor.
 */
#ifndef MICRO_OS_KERNEL_PORT_CPU_H
#define MICRO_OS_KERNEL_PORT_CPU_H

#include "kernel/types.h"

/*
 * The number of cores the kernel may use: the cores the port has started and
 * can take a task, not the cores the firmware reports. A machine whose second
 * core is not up yet answers 1, and the port says so in its own boot log; the
 * kernel would put a task on a core that never runs it otherwise.
 */
k_u32_t k_port_cpu_count(void);

/* The core that is asking. Any number below k_port_cpu_count(). */
k_u32_t k_port_cpu_id(void);

/*
 * Tell a core that its run queue has a task it did not have a moment ago.
 *
 * The scheduler calls this after it has put a task in the queue of a core
 * other than the one that runs the caller, and it calls it with the scheduler
 * lock dropped: the core that takes the interrupt looks at the same queue
 * under the same lock. A port with a wake-up interrupt of its own (the IPI of
 * x86-64) sends it, and the task starts at once instead of waiting for that
 * core's next tick; a port without one does nothing, and the next tick of the
 * core is what starts the task, which is the behaviour of every port that
 * predates the call.
 *
 * The call is a hint and not a promise: a core that is already running a task
 * of a higher priority may keep it, and the queue is the truth either way.
 */
void k_port_cpu_wake(k_u32_t cpu);

/*
 * Debug hooks for the hang watchdog. The defaults return 0/false: a port that
 * has no such information is still diagnosable, only with fewer fields. The
 * calls must be lock-free and safe to use from a diagnostic path.
 */
k_u32_t k_port_cpu_critical_depth(k_u32_t cpu);
k_u64_t k_port_cpu_saved_rip(k_u32_t cpu);
k_u64_t k_port_debug_tsc(void);

#endif /* MICRO_OS_KERNEL_PORT_CPU_H */
