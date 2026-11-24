/*
 * Micro OS — the wait channels (kernel/wait.h).
 *
 * One record per waiter, in a table of the size of the table of the tasks,
 * and one lock: the critical section of the port, which the scheduler also
 * takes, so that the check of a state and the removal of the task from the
 * run queue cannot be cut by the wakeup of the other side. A wakeup that
 * arrives while the waiter is still running is not lost either: it is
 * remembered in the record and the sleep does not begin (the flag is looked
 * at under the same lock that marks the task as sleeping).
 *
 * The wake is immediate. In 1.8 it was a move of the wake tick to the present
 * and the timer did the rest, so an object answered in one tick - 1.006 ms of
 * a pipe on x86-64 (docs/bench.md). Here the woken task leaves the sleeping
 * table at once, enters the run queue of its core, and the port is told to
 * cut the sleep it is in (the host) or to send the interrupt of the other
 * core (x86-64, ADR 0012); the round trip of a pipe is therefore the two
 * handoffs and not a millisecond of the clock.
 */
#include "kernel/wait.h"

#include "kernel/cpu.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/signal.h"

/*
 * The sleep of a wait on a port whose sleep cannot be cut.
 *
 * A port that answers false to k_port_task_sleep_cuttable() sleeps for the
 * whole time it is asked for and nothing outside the task can end it early
 * (the board, whose tasks are FreeRTOS tasks in vTaskDelay). The wait of such
 * a port is the wait of 1.8: the task looks at its record every ten
 * milliseconds, so a state that becomes true is seen within the chunk. It is
 * not a lost wakeup - the arm closes that window - only the granularity of a
 * port with no door of its own, and docs/debts.md keeps it as one.
 */
#define WAIT_CHUNK_MS 10u

/*
 * The longest single sleep of an infinite wait on a port that can cut one.
 *
 * A wake ends the sleep long before this, so the number is a backstop and not
 * a latency: it keeps an unbounded wait from being a tick count that wraps
 * and bounds the damage of a bug in a port's door. A second is short enough
 * for a stuck task to be looked at and long enough to cost nothing when
 * nothing is wrong.
 */
#define WAIT_SLEEP_LONG_MS 1000u

struct k_waiter {
    bool used;
    bool woken;
    const void *channel;
    const void *second; /* the second channel of the same wait, or NULL */
    struct task *task;
};

static struct k_waiter g_waiters[TASK_MAX_TASKS];

const char k_wait_channel_pipes;
const char k_wait_channel_console;

/* The record of a task, NULL when it waits on nothing (callers hold the
 * lock). */
static struct k_waiter *waiter_of_locked(const struct task *t)
{
    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(g_waiters); i++) {
        if (g_waiters[i].used && g_waiters[i].task == t) {
            return &g_waiters[i];
        }
    }

    return NULL;
}

/* --- arming --- */

bool k_wait_arm(const void *channel, const void *second)
{
    struct task *self = task_self();
    struct k_waiter *w = NULL;

    if (self == NULL) {
        return false;
    }

    k_cpu_lock();

    w = waiter_of_locked(self);

    if (w == NULL) {
        k_size_t i;

        for (i = 0; i < K_ARRAY_SIZE(g_waiters); i++) {
            if (!g_waiters[i].used) {
                w = &g_waiters[i];
                break;
            }
        }
    }

    if (w != NULL) {
        w->used = true;
        w->task = self;
        w->channel = channel;
        w->second = second;
        /*
         * A wake that arrived before this line belongs to the state the
         * caller has just looked at: it is seen by the look and not by the
         * sleep, and the record starts clean.
         */
        w->woken = false;
    }

    k_cpu_unlock();

    return w != NULL;
}

void k_wait_disarm(void)
{
    struct task *self = task_self();
    struct k_waiter *w;

    if (self == NULL) {
        return;
    }

    k_cpu_lock();

    w = waiter_of_locked(self);

    if (w != NULL) {
        w->used = false;
        w->woken = false;
        w->channel = NULL;
        w->second = NULL;
        w->task = NULL;
    }

    k_cpu_unlock();
}

/* --- sleeping --- */

int k_wait_sleep(k_u32_t timeout_ms)
{
    struct task *self = task_self();
    struct k_waiter *w;
    k_u64_t deadline;
    bool cuttable;

    if (timeout_ms == 0) {
        return K_WAIT_TIMEOUT;
    }

    if (self == NULL) {
        /*
         * The boot and the tests may wait outside of a task: there is no run
         * queue to leave, so the wait is the sleep of the port and nothing
         * can wake it early.
         */
        if (timeout_ms != K_WAIT_INFINITE) {
            k_port_task_sleep_ms(timeout_ms);
        }

        return K_WAIT_TIMEOUT;
    }

    cuttable = k_port_task_sleep_cuttable();
    deadline = (timeout_ms == K_WAIT_INFINITE) ? 0 : k_sched_uptime_ms() + (k_u64_t)timeout_ms;

    for (;;) {
        k_u32_t chunk;

        k_cpu_lock();
        w = waiter_of_locked(self);
        if (w != NULL && w->woken) {
            k_cpu_unlock();
            return K_WAIT_WOKEN;
        }
        k_cpu_unlock();

        /*
         * A signal the task has to answer ends the wait as well
         * (task_signal_interrupts), which is what makes it interruptible.
         */
        if (task_signal_interrupts(self)) {
            return K_WAIT_SIGNAL;
        }

        if (timeout_ms == K_WAIT_INFINITE) {
            chunk = cuttable ? WAIT_SLEEP_LONG_MS : WAIT_CHUNK_MS;
        } else {
            k_u64_t now = k_sched_uptime_ms();
            k_u64_t left;

            if (now >= deadline) {
                return K_WAIT_TIMEOUT;
            }

            left = deadline - now;
            chunk = (k_u32_t)K_MIN(left, (k_u64_t)(cuttable ? WAIT_SLEEP_LONG_MS : WAIT_CHUNK_MS));
        }

        /*
         * The sleep does not begin when the record has been woken between
         * the look above and the mark of the task as sleeping: the check of
         * the flag and the removal from the run queue are one section, and
         * the wake sets both under the same lock.
         *
         * A caller without a record - only a task table smaller than its
         * waiters could do it - has nothing that can wake it: the sleep is
         * the plain one of the scheduler and the caller looks at its object
         * again when it ends, which is the bounded wait of 1.8.
         */
        if (!k_sched_sleep_unless(chunk, (w != NULL) ? &w->woken : NULL)) {
            return K_WAIT_WOKEN;
        }
    }
}

/* --- the wake --- */

/*
 * Wake the waiters of one channel. The answer is the number of the records
 * that were touched, which the tests read; the tasks that left the sleeping
 * table are put in `poke`, so that the port is told after the lock is
 * dropped (the host's door takes a mutex of its own and must not be called
 * with the scheduler lock held).
 *
 * A waiter that is asleep is woken now: it leaves the sleeping table, becomes
 * READY in the queue of its core and that core is told - the scheduler
 * answers which of the two happened. A waiter that is still running has not
 * entered its sleep yet: the record says so and the sleep is skipped when the
 * task gets there (k_sched_sleep_unless).
 */
static k_size_t wake_locked(const void *channel, bool all, struct task **poke, k_size_t room,
                            k_size_t *poked)
{
    struct task *self = task_self();
    k_size_t count = 0;
    k_size_t i;

    *poked = 0;

    for (i = 0; i < K_ARRAY_SIZE(g_waiters); i++) {
        struct k_waiter *w = &g_waiters[i];

        if (!w->used || w->task == NULL || w->task == self) {
            continue;
        }

        if (!all && w->channel != channel && w->second != channel) {
            continue;
        }

        w->woken = true;
        count++;

        if (k_sched_wake_task_locked(w->task) && *poked < room) {
            poke[(*poked)++] = w->task;
        }
    }

    return count;
}

static void wake_and_flush(const void *channel, bool all)
{
    struct task *poke[TASK_MAX_TASKS];
    k_size_t poked = 0;
    k_size_t i;

    k_cpu_lock();
    (void)wake_locked(channel, all, poke, K_ARRAY_SIZE(poke), &poked);
    k_cpu_unlock();

    /*
     * The sleep of the port first: it is the shortest path to a task that is
     * runnable again, and the interrupt of the other core is next.
     */
    for (i = 0; i < poked; i++) {
        k_port_task_wake(poke[i]);
    }

    k_sched_wake_flush();
}

void k_wait_wake(const void *channel)
{
    wake_and_flush(channel, false);
}

void k_wait_wake_all(void)
{
    wake_and_flush(NULL, true);
}

/* --- the bookkeeping the tests and /proc read --- */

k_size_t k_wait_count(const void *channel)
{
    k_size_t count = 0;
    k_size_t i;

    k_cpu_lock();

    for (i = 0; i < K_ARRAY_SIZE(g_waiters); i++) {
        if (g_waiters[i].used && g_waiters[i].channel == channel) {
            count++;
        }
    }

    k_cpu_unlock();

    return count;
}

k_size_t k_wait_total(void)
{
    k_size_t count = 0;
    k_size_t i;

    k_cpu_lock();

    for (i = 0; i < K_ARRAY_SIZE(g_waiters); i++) {
        if (g_waiters[i].used) {
            count++;
        }
    }

    k_cpu_unlock();

    return count;
}

void k_wait_forget(const struct task *t)
{
    k_size_t i;

    if (t == NULL) {
        return;
    }

    k_cpu_lock();

    for (i = 0; i < K_ARRAY_SIZE(g_waiters); i++) {
        if (g_waiters[i].used && g_waiters[i].task == t) {
            g_waiters[i].used = false;
            g_waiters[i].woken = false;
            g_waiters[i].channel = NULL;
            g_waiters[i].second = NULL;
            g_waiters[i].task = NULL;
        }
    }

    k_cpu_unlock();
}
