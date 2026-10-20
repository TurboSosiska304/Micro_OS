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

/* Time slice of a task in ticks (1 tick = 1 ms). */
#define SCHED_QUANTUM_DEFAULT 10
#define SCHED_QUANTUM_MIN 1

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

/*
 * Time slice: when the running task has used up its quantum it is moved to
 * the end of its priority queue, so that the tasks of the same priority take
 * turns (Linux SCHED_RR style).
 */
void k_sched_set_quantum(k_u32_t ticks);
k_u32_t k_sched_quantum(void);
k_u32_t k_sched_quantum_left(void);

/* Internal: called by the task trampoline. */
void k_sched_task_ready(struct task *t);
void k_sched_task_running(struct task *t);
void k_sched_task_terminated(struct task *t);

#endif /* MICRO_OS_KERNEL_SCHED_H */
