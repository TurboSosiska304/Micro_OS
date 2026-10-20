/*
 * Micro OS — task scheduling.
 *
 * The core keeps a run queue per priority level and a queue of sleeping
 * tasks. On ESP32 the actual context switching is performed by FreeRTOS
 * (see ADR 0002); the structures below are the logical model of the
 * scheduler and the source of truth for /proc.
 */
#ifndef MICRO_OS_KERNEL_SCHED_H
#define MICRO_OS_KERNEL_SCHED_H

#include "kernel/task.h"
#include "kernel/types.h"

#define SCHED_READY_SLOTS 8

/* Initialise the scheduler. Must be called before the first task_create(). */
void k_sched_init(void);

/* Advance the time base by one tick (1 ms). Called by the port timer. */
void k_sched_tick(void);

/* Number of ticks since k_sched_init(). */
k_u32_t k_sched_ticks(void);

/* Milliseconds since k_sched_init(). */
k_u64_t k_sched_uptime_ms(void);

/* Give up the CPU. */
void k_sched_yield(void);

/* Sleep the current task for the given number of milliseconds. */
void k_sched_sleep_ms(k_u32_t ms);

/* Block/unblock a task (waiting for an event, e.g. a queue or a device). */
void k_sched_block(struct task *t);
void k_sched_unblock(struct task *t);

/* The highest priority task that is ready to run (NULL if there is none). */
struct task *k_sched_pick_next(void);

/* Statistics. */
k_size_t k_sched_ready_count(void);
k_size_t k_sched_sleeping_count(void);
k_size_t k_sched_blocked_count(void);

/* Internal: called by the task trampoline. */
void k_sched_task_ready(struct task *t);
void k_sched_task_running(struct task *t);
void k_sched_task_terminated(struct task *t);

#endif /* MICRO_OS_KERNEL_SCHED_H */
