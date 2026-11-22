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

#endif /* MICRO_OS_KERNEL_PORT_CPU_H */
