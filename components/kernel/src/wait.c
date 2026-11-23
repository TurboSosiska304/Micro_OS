/*
 * Micro OS — the wait channels (kernel/wait.h).
 *
 * One record per waiter, in a table of the size of the table of the tasks,
 * and one lock: the critical section of the port, which the scheduler also
 * takes, so that the check of a state and the removal of the task from the
 * run queue cannot be cut by the wakeup of the other side. A wakeup that
 * arrives while the waiter is still running is not lost either: it is
 * remembered in the record and the waiter sees it before it sleeps.
 */
#include "kernel/wait.h"

#include "kernel/cpu.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/signal.h"

/*
 * The longest single sleep of a wait.
 *
 * The port of x86-64 can cut a sleep: its loop looks at the state of the task
 * and the tick of the timer moves the wake to the present, so a wakeup of an
 * object is answered within a tick and the chunk is never used up. A port
 * whose tasks are threads of another scheduler - the host, the board - has no
 * such door: its sleep runs to the end and nothing outside the thread can end
 * it. The chunk is what keeps a wait of those ports short: the task sleeps,
 * looks at its record, and sleeps again, so a state that becomes true is seen
 * within the chunk and the task still costs no processor time while it
 * waits.
 */
#define WAIT_CHUNK_MS 10u

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

/*
 * Wake the waiters of one channel. The answer is the number of the records
 * that were touched, which the tests read.
 *
 * A waiter that is asleep is woken by the scheduler at its next tick: moving
 * the wake tick to the present is the whole of the request, and the tick of
 * the timer is the point where the task becomes runnable again. A waiter that
 * is still running has not entered its sleep yet: the record says so and the
 * sleep is skipped when the task gets there.
 */
static k_size_t wake_locked(const void *channel, bool all)
{
    struct task *self = task_self();
    k_u32_t now = k_sched_ticks();
    k_size_t count = 0;
    k_size_t i;

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

        /*
         * The task is in the queue of the sleeping tasks: the tick moves it
         * back to the run queue when the wake tick has come. A task that is
         * blocked, ready or running is left alone; its record is enough.
         */
        if (w->task->state == TASK_STATE_SLEEPING && w->task->wake_tick > now) {
            w->task->wake_tick = now;
        }
    }

    return count;
}

void k_wait_wake(const void *channel)
{
    k_cpu_lock();
    (void)wake_locked(channel, false);
    k_cpu_unlock();
}

void k_wait_wake_all(void)
{
    k_cpu_lock();
    (void)wake_locked(NULL, true);
    k_cpu_unlock();
}

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

int k_wait_block(const void *channel, const void *second, k_u32_t timeout_ms)
{
    struct task *self = task_self();
    struct k_waiter *slot;
    k_u64_t deadline;
    k_u32_t chunk;
    int answer = K_WAIT_TIMEOUT;

    if (self == NULL) {
        /*
         * The boot and the tests may wait outside of a task: there is no run
         * queue to leave, so the wait is the sleep of the port and nothing
         * can wake it early.
         */
        if (timeout_ms != K_WAIT_INFINITE && timeout_ms != 0) {
            k_port_task_sleep_ms(timeout_ms);
        }

        return K_WAIT_TIMEOUT;
    }

    if (timeout_ms == 0) {
        return K_WAIT_TIMEOUT;
    }

    k_cpu_lock();

    slot = waiter_of_locked(self);
    if (slot == NULL) {
        k_size_t i;

        for (i = 0; i < K_ARRAY_SIZE(g_waiters); i++) {
            if (!g_waiters[i].used) {
                slot = &g_waiters[i];
                slot->used = true;
                slot->task = self;
                break;
            }
        }
    }

    if (slot == NULL) {
        /* Every record is taken: the wait is a sleep without a wakeup, which
         * is a wrong answer of a system whose table of the tasks is smaller
         * than its waiters. It cannot happen while a waiter is a task. */
        k_cpu_unlock();
        kerror("wait: no record for a waiter");
        k_sched_sleep_ms(timeout_ms == K_WAIT_INFINITE ? WAIT_CHUNK_MS : timeout_ms);
        return K_WAIT_TIMEOUT;
    }

    slot->channel = channel;
    slot->second = second;
    slot->woken = false;

    k_cpu_unlock();

    deadline = (timeout_ms == K_WAIT_INFINITE) ? 0 : k_sched_uptime_ms() + (k_u64_t)timeout_ms;

    for (;;) {
        k_u64_t now;

        k_cpu_lock();
        if (slot->woken) {
            k_cpu_unlock();
            answer = K_WAIT_WOKEN;
            break;
        }
        k_cpu_unlock();

        /*
         * The sleep is the scheduler's: the task leaves the run queue and
         * the tick puts it back when the deadline comes or when a wakeup
         * moves the tick to the present. A signal the task has to answer
         * ends the sleep as well (task_signal_interrupts), which is what
         * makes a wait interruptible.
         */
        if (timeout_ms == K_WAIT_INFINITE) {
            chunk = WAIT_CHUNK_MS;
        } else {
            now = k_sched_uptime_ms();

            if (now >= deadline) {
                answer = K_WAIT_TIMEOUT;
                break;
            }

            chunk = (k_u32_t)(deadline - now);
            if (chunk > WAIT_CHUNK_MS) {
                chunk = WAIT_CHUNK_MS;
            }
        }

        k_sched_sleep_ms(chunk);

        k_cpu_lock();
        if (slot->woken) {
            k_cpu_unlock();
            answer = K_WAIT_WOKEN;
            break;
        }
        k_cpu_unlock();

        if (task_signal_interrupts(self)) {
            answer = K_WAIT_SIGNAL;
            break;
        }

        if (timeout_ms != K_WAIT_INFINITE && k_sched_uptime_ms() >= deadline) {
            answer = K_WAIT_TIMEOUT;
            break;
        }
    }

    k_cpu_lock();
    slot->used = false;
    slot->woken = false;
    slot->channel = NULL;
    slot->second = NULL;
    slot->task = NULL;
    k_cpu_unlock();

    return answer;
}
