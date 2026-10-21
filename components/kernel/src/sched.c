#include "kernel/sched.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/task.h"

typedef struct {
    struct task *slots[SCHED_READY_CAPACITY];
    k_u8_t head;
    k_u8_t tail;
    k_u8_t count;
} ready_queue_t;

static ready_queue_t g_ready[TASK_PRIO_MAX + 1];
static struct task *g_sleeping[TASK_MAX_TASKS];
static k_size_t g_sleeping_count;
static k_u32_t g_ticks;
static k_u32_t g_quantum = SCHED_QUANTUM_DEFAULT;
static bool g_initialised;

/* --- run queue primitives (callers hold the lock) --- */

/* Is the task already in the queue? (callers hold the lock) */
static bool ready_contains(const ready_queue_t *q, const struct task *t)
{
    k_u8_t i;

    for (i = 0; i < q->count; i++) {
        k_u8_t idx = (k_u8_t)((q->head + i) % SCHED_READY_CAPACITY);

        if (q->slots[idx] == t) {
            return true;
        }
    }

    return false;
}

static void ready_enqueue(struct task *t)
{
    ready_queue_t *q = &g_ready[t->prio];

    /* A task must be in the queue at most once. */
    if (ready_contains(q, t)) {
        return;
    }

    if (q->count >= SCHED_READY_CAPACITY) {
        /*
         * The queue is dimensioned for every task of the system, so this can
         * only happen if a task was added twice. The task is not dropped:
         * it stays ready and the situation is reported.
         */
        kerror("sched: the run queue of the priority %u is full", (unsigned)t->prio);
        return;
    }

    q->slots[q->tail] = t;
    q->tail = (k_u8_t)((q->tail + 1) % SCHED_READY_CAPACITY);
    q->count++;
}

static void ready_remove(struct task *t)
{
    ready_queue_t *q = &g_ready[t->prio];
    k_u8_t i;

    for (i = 0; i < q->count; i++) {
        k_u8_t idx = (k_u8_t)((q->head + i) % SCHED_READY_CAPACITY);
        if (q->slots[idx] == t) {
            for (; i + 1 < q->count; i++) {
                q->slots[(q->head + i) % SCHED_READY_CAPACITY] =
                    q->slots[(q->head + i + 1) % SCHED_READY_CAPACITY];
            }
            q->count--;
            q->tail = (k_u8_t)((q->head + q->count) % SCHED_READY_CAPACITY);
            return;
        }
    }
}

static void ready_move_to_tail(struct task *t)
{
    ready_remove(t);
    ready_enqueue(t);
}

/* --- public API --- */

void k_sched_init(void)
{
    k_port_enter_critical();
    memset(g_ready, 0, sizeof(g_ready));
    memset(g_sleeping, 0, sizeof(g_sleeping));
    g_sleeping_count = 0;
    g_ticks = 0;
    g_initialised = true;
    k_port_exit_critical();
}

void k_sched_tick(void)
{
    k_size_t i = 0;

    k_port_enter_critical();

    g_ticks++;

    /* Wake up the tasks whose timeout has expired. */
    while (i < g_sleeping_count) {
        struct task *t = g_sleeping[i];

        if (t->wake_tick <= g_ticks && t->state == TASK_STATE_SLEEPING) {
            g_sleeping[i] = g_sleeping[g_sleeping_count - 1];
            g_sleeping_count--;
            t->state = t->wake_state;
            if (t->state == TASK_STATE_READY || t->state == TASK_STATE_RUNNING) {
                ready_enqueue(t);
            }
            continue;
        }
        i++;
    }

    /*
     * Round robin: the running task loses the CPU when its time slice is
     * over and goes to the end of the queue of its priority level.
     */
    if (g_quantum > 0) {
        struct task *current = k_port_task_current();

        if (current != NULL && current->state == TASK_STATE_RUNNING) {
            if (current->quantum_left == 0) {
                current->quantum_left = g_quantum;
            }

            current->quantum_left--;

            if (current->quantum_left == 0) {
                current->runtime_ticks++;
                ready_move_to_tail(current);
                current->state = TASK_STATE_READY;
            }
        }
    }

    k_port_exit_critical();
}

void k_sched_set_quantum(k_u32_t ticks)
{
    k_port_enter_critical();
    g_quantum = (ticks < SCHED_QUANTUM_MIN) ? SCHED_QUANTUM_MIN : ticks;
    k_port_exit_critical();
}

k_u32_t k_sched_quantum(void)
{
    k_u32_t quantum;

    k_port_enter_critical();
    quantum = g_quantum;
    k_port_exit_critical();

    return quantum;
}

k_u32_t k_sched_quantum_left(void)
{
    struct task *self = task_self();
    k_u32_t left = 0;

    if (self != NULL) {
        k_port_enter_critical();
        left = self->quantum_left;
        k_port_exit_critical();
    }

    return left;
}

k_u32_t k_sched_ticks(void)
{
    k_u32_t ticks;

    k_port_enter_critical();
    ticks = g_ticks;
    k_port_exit_critical();

    return ticks;
}

k_u64_t k_sched_uptime_ms(void)
{
    return (k_u64_t)k_sched_ticks();
}

void k_sched_yield(void)
{
    struct task *self = task_self();

    if (self != NULL) {
        k_port_enter_critical();
        if (self->state == TASK_STATE_RUNNING) {
            self->state = TASK_STATE_READY;
        }
        ready_move_to_tail(self);
        k_port_exit_critical();
    }

    k_port_task_yield();
}

void k_sched_sleep_ms(k_u32_t ms)
{
    struct task *self = task_self();
    task_state_t previous;
    k_size_t i;

    if (self == NULL) {
        /* Not running in a task context: just wait. */
        k_port_task_sleep_ms(ms);
        return;
    }

    k_port_enter_critical();
    previous = self->state;
    self->state = TASK_STATE_SLEEPING;
    /*
     * A blocked task stays blocked, everything else returns to the run
     * queue: after the timeout the task is runnable again.
     */
    self->wake_state = (previous == TASK_STATE_BLOCKED) ? TASK_STATE_BLOCKED : TASK_STATE_READY;
    self->wake_tick = g_ticks + ms;
    /* A sleeping task is not in the run queue. */
    ready_remove(self);
    if (g_sleeping_count < TASK_MAX_TASKS) {
        g_sleeping[g_sleeping_count++] = self;
    }
    k_port_exit_critical();

    k_port_task_sleep_ms(ms);

    k_port_enter_critical();

    /* The task may already have been woken up by the tick handler. */
    for (i = 0; i < g_sleeping_count; i++) {
        if (g_sleeping[i] == self) {
            g_sleeping[i] = g_sleeping[g_sleeping_count - 1];
            g_sleeping_count--;
            break;
        }
    }

    /*
     * A block or an unblock that arrived while the task was sleeping is
     * applied now: the request must not get lost.
     */
    if (self->pending_wake) {
        self->pending_wake = false;
        self->pending_block = false;
        self->state = TASK_STATE_READY;
        ready_enqueue(self);
    } else if (self->pending_block) {
        self->pending_block = false;
        self->state = TASK_STATE_BLOCKED;
    } else if (self->state == TASK_STATE_SLEEPING) {
        if (previous == TASK_STATE_BLOCKED) {
            /* A blocked task stays blocked after the timeout. */
            self->state = TASK_STATE_BLOCKED;
        } else {
            self->state = TASK_STATE_READY;
            ready_enqueue(self);
        }
    }

    k_port_exit_critical();
}

void k_sched_block(struct task *t)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();

    if (t->state == TASK_STATE_SLEEPING) {
        /* The task waits with a timeout: remember the request. */
        t->pending_block = true;
        t->pending_wake = false;
    } else if (t->state == TASK_STATE_READY || t->state == TASK_STATE_RUNNING) {
        ready_remove(t);
        t->state = TASK_STATE_BLOCKED;
    }

    k_port_exit_critical();
}

void k_sched_unblock(struct task *t)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();

    if (t->state == TASK_STATE_SLEEPING) {
        /* The task waits with a timeout: remember the request. */
        t->pending_wake = true;
        t->pending_block = false;
    } else if (t->state == TASK_STATE_BLOCKED) {
        t->state = TASK_STATE_READY;
        ready_enqueue(t);
    }

    k_port_exit_critical();
}

struct task *k_sched_pick_next(void)
{
    struct task *next = NULL;
    int prio;

    k_port_enter_critical();
    for (prio = TASK_PRIO_MAX; prio >= TASK_PRIO_MIN; prio--) {
        ready_queue_t *q = &g_ready[prio];
        if (q->count > 0) {
            next = q->slots[q->head];
            break;
        }
    }
    k_port_exit_critical();

    return next;
}

k_size_t k_sched_ready_count(void)
{
    k_size_t count = 0;
    int prio;

    k_port_enter_critical();
    for (prio = TASK_PRIO_MIN; prio <= TASK_PRIO_MAX; prio++) {
        count += g_ready[prio].count;
    }
    k_port_exit_critical();

    return count;
}

k_size_t k_sched_sleeping_count(void)
{
    k_size_t count;

    k_port_enter_critical();
    count = g_sleeping_count;
    k_port_exit_critical();

    return count;
}

k_size_t k_sched_blocked_count(void)
{
    return task_count_by_state(TASK_STATE_BLOCKED);
}

k_size_t k_sched_validate(void)
{
    k_size_t problems = 0;
    k_size_t i;
    int prio;

    k_port_enter_critical();

    /* Every ready task has to be in the queue of its priority level. */
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = task_get_by_index_unlocked(i);

        if (t == NULL) {
            continue;
        }

        if (t->state == TASK_STATE_READY && !ready_contains(&g_ready[t->prio], t)) {
            kerror("sched: the task '%s' is ready, but not in the queue", t->name);
            problems++;
        }
    }

    /* Every task in a queue has to be ready. */
    for (prio = TASK_PRIO_MIN; prio <= TASK_PRIO_MAX; prio++) {
        ready_queue_t *q = &g_ready[prio];
        k_u8_t i2;

        for (i2 = 0; i2 < q->count; i2++) {
            k_u8_t idx = (k_u8_t)((q->head + i2) % SCHED_READY_CAPACITY);
            struct task *t = q->slots[idx];

            if (t == NULL || t->state != TASK_STATE_READY) {
                kerror("sched: the queue of the priority %d holds a task that is not ready", prio);
                problems++;
            }
        }
    }

    k_port_exit_critical();

    return problems;
}

void k_sched_task_ready(struct task *t)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();
    t->state = TASK_STATE_READY;
    ready_enqueue(t);
    k_port_exit_critical();
}

void k_sched_task_running(struct task *t)
{
    if (t == NULL) {
        return;
    }

    k_port_enter_critical();
    t->state = TASK_STATE_RUNNING;
    t->runtime_ticks++;
    k_port_exit_critical();
}

void k_sched_task_terminated(struct task *t)
{
    k_size_t i;

    if (t == NULL) {
        return;
    }

    k_port_enter_critical();

    /* The task is not in the run queue any more. */
    ready_remove(t);

    for (i = 0; i < g_sleeping_count; i++) {
        if (g_sleeping[i] == t) {
            g_sleeping[i] = g_sleeping[g_sleeping_count - 1];
            g_sleeping_count--;
            break;
        }
    }

    t->state = TASK_STATE_ZOMBIE;

    k_port_exit_critical();
}
