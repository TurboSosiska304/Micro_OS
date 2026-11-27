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

/*
 * The number of entries of the run queue of one priority level.
 *
 * It has to be at least TASK_MAX_TASKS: every task of the system can be
 * ready at the same time and belong to the same priority level. A smaller
 * queue would silently drop tasks, they would stay "ready" but would never
 * be picked up again.
 */
#define SCHED_READY_CAPACITY TASK_MAX_TASKS

/* Time slice of a task in ticks (1 tick = 1 ms). */
#define SCHED_QUANTUM_DEFAULT 10
#define SCHED_QUANTUM_MIN 1

/* Initialise the scheduler. Must be called before the first task_create(). */
void k_sched_init(void);

/* Advance the time base by one tick (1 ms). Called by the port timer. */
void k_sched_tick(void);

/* Number of ticks since k_sched_init(). */
k_u32_t k_sched_ticks(void);

/* The ticks one core has taken since the boot (0 for a core that is not
 * there). With KERNEL_SMP off only the core 0 has any. */
k_u32_t k_sched_cpu_ticks(k_u32_t cpu);

/* The cores the scheduler has (1 without KERNEL_SMP). */
k_u32_t k_sched_cpu_count(void);

/* Milliseconds since k_sched_init(). */
k_u64_t k_sched_uptime_ms(void);

/* Give up the CPU. */
void k_sched_yield(void);

/* Sleep the current task for the given number of milliseconds. Never less:
 * the sleep is a loop on the clock of the port (see below). */
void k_sched_sleep_ms(k_u32_t ms);

/*
 * Sleep the current task for at least `ms` milliseconds, without beginning the
 * sleep when `stop` is already true. The look at the flag and the removal from
 * the run queue are one critical section, so a wakeup that sets the flag in
 * the window between the caller's look and this call is not slept on - that
 * window is the lost wakeup of 1.8 (docs/debts.md, K15).
 *
 * A sleep is never shorter than its argument, whatever wakes the port: the
 * deadline is a number of microseconds of the clock of the port, the task
 * sleeps what is left in a loop, and a nudge that cut a round (the door of a
 * port, a wake that lost its race) or a tick that fired early only makes the
 * loop sleep the rest again. A tick is the unit of the scheduler and a
 * millisecond of the caller is the unit of the port; on the host, whose clock
 * is a thread of its own, the two part company under a slow runner.
 *
 * `stop` (the record of a wait) is the reason a wait ends early, with a
 * signal the task has to answer; a block or an unblock that arrived while the
 * task slept is the other. A plain sleep is not cut by a signal: the delay of
 * a login that a signal could shorten is a delay an attacker chooses.
 *
 * Answers true when the task slept, false when it did not because the flag
 * was set. The flag is written under the scheduler lock by whoever wakes the
 * task (kernel/wait.c), which is what makes the plain read of it safe.
 */
bool k_sched_sleep_unless(k_u32_t ms, const bool *stop);

/*
 * Wake a task that is asleep, now: it leaves the sleeping table, becomes what
 * its sleep was going to make it (READY for a wait), enters the run queue of
 * its core and that core is told - the interrupt of the port when it is
 * another one. The caller holds the scheduler lock; the interrupt goes out
 * after the caller has dropped it, with k_sched_wake_flush().
 *
 * Answers true when the task was runnable after the call (it left the
 * sleeping table), false when it was not sleeping or its sleep was a block.
 */
bool k_sched_wake_task_locked(struct task *t);

/* Send the interrupts the hand-overs of k_sched_wake_task_locked() asked for.
 * Called with the scheduler lock dropped. */
void k_sched_wake_flush(void);

/* Block/unblock a task (waiting for an event, e.g. a queue or a device). */
void k_sched_block(struct task *t);
void k_sched_unblock(struct task *t);

/*
 * Stop and continue a task. A stopped task is in no run queue and no sleep
 * table; SIGCONT is the only operation that makes it ready again. The stop
 * also remembers that a sleeping task must not come back early when the port
 * ends the sleep it was in.
 */
bool k_sched_stop(struct task *t);
bool k_sched_continue(struct task *t);

/* Look at the queues: the task this core would run next, not taken. This is
 * the peek of the tests and the diagnostics; a port that switches must use
 * k_sched_claim_next(), because a choice another core can repeat is two cores
 * on one saved stack pointer. */
struct task *k_sched_pick_next(void);

/*
 * Choose the task this core runs next and mark it as running here, under the
 * scheduler lock. The task leaves the ready set of its queue and becomes
 * RUNNING on the calling core in one critical section, so no other core can
 * claim it between the choice and the switch. Returns the task that now runs
 * here, or NULL when nothing is runnable (or the chosen task has no port
 * context).
 */
struct task *k_sched_claim_next(void);

/* Statistics. */
k_size_t k_sched_ready_count(void);
k_size_t k_sched_ready_count_cpu(k_u32_t cpu);
k_size_t k_sched_sleeping_count(void);
k_size_t k_sched_blocked_count(void);

/*
 * The deadlines of the sleeping table, for a diagnosis of a wait that did
 * not end. The record is a snapshot under the scheduler lock, so a port can
 * print it without holding that lock while it writes to a console. The
 * number returned is the number of records written.
 */
struct k_sched_sleeper {
    k_u32_t pid;
    k_u32_t wake_tick;
    k_u32_t cpu;
    k_u8_t state;
};

k_size_t k_sched_sleepers(struct k_sched_sleeper *out, k_size_t max);

/*
 * Move a task to the queue of another core. Only the scheduler decides where
 * a task belongs; this call is the one the scheduler and the tests use. A
 * task that is running is refused (-1): it is not moved in the middle of its
 * slice (ADR 0012). A blocked or sleeping task remembers the core it will be
 * woken on; a ready one changes its queue. Returns 0, or -1 for a task with
 * no record, a core that is not there, or a running task.
 */
int k_sched_migrate(struct task *t, k_u32_t cpu);

/* The core of the run queue of a task (0 without KERNEL_SMP). */
k_u32_t k_sched_task_cpu(const struct task *t);

/*
 * The tasks the scheduler has handed to another core since the boot - each
 * one was followed by the wake call of the port, k_port_cpu_wake() - and the
 * last of them: the core that got it and the ticks of that core at the moment
 * of the hand-over. A port with a wake-up interrupt prints the pair beside its
 * own count of the tasks the interrupt started, and a task that starts at the
 * same tick started before that core's timer could have offered it.
 */
k_u32_t k_sched_remote_wakes(void);
void k_sched_last_remote_wake(k_u32_t *cpu, k_u32_t *tick);

/*
 * Check the invariants of the run queues: a task that is ready has to be in
 * the queue of its priority level, and a task in a queue has to be runnable
 * there - ready, or running in that queue, which is how the switch of this
 * kernel keeps the task it runs - and in the queue of its own core. Returns
 * the number of broken invariants (0 when everything is fine).
 */
k_size_t k_sched_validate(void);

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
