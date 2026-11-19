/*
 * Micro OS — platform port: the clock.
 *
 * The kernel counts its own ticks; the port only says how much real time
 * has passed since the boot. There is no wall clock here on purpose: the
 * date belongs to the RTC driver, which is a driver and not a port duty.
 */
#ifndef MICRO_OS_KERNEL_PORT_TIME_H
#define MICRO_OS_KERNEL_PORT_TIME_H

#include "kernel/types.h"

/* Milliseconds since boot. */
k_u64_t k_port_uptime_ms(void);

/* Microseconds since the boot (for the benchmarks). */
k_u64_t k_port_uptime_us(void);

#endif /* MICRO_OS_KERNEL_PORT_TIME_H */
