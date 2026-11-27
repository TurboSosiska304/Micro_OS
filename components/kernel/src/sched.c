#include "kernel/sched.h"

#include <string.h>

#include "kernel/cpu.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/signal.h"
#include "kernel/task.h"

typedef struct {
    struct task *slots[SCHED_READY_CAPACITY];
    k_u8_t head;
    k_u8_t tail;
    k_u8_t count;
} ready_queue_t;

/*
 * The run queues of one core. A task belongs to the queue of the core it is
 * being run on (or is about to be), and the scheduler is the only owner of
 * that field: a task in the queue of a core is ready there and nowhere else.
 * With KERNEL_SMP off the array has one entry and the whole file is the
 * scheduler of 1.0.
 */
struct sched_cpu {
    ready_queue_t ready[TASK_PRIO_MAX + 1];
};

static struct sched_cpu g_queues[KERNEL_CPUS_MAX];
static struct task *g_sleeping[TASK_MAX_TASKS];
static k_size_t g_sleeping_count;
static k_u32_t g_ticks;
static k_u32_t g_quantum = SCHED_QUANTUM_DEFAULT;
static bool g_initialised;

/* The next core of a task that has just been created, round robin. */
static k_u32_t g_next_cpu;

/*
 * The cores the scheduler has handed a task to and not told yet, one bit each,
 * and the count of the hand-overs since the boot.
 *
 * The mark is set under the scheduler lock, where the task is put in the queue
 * of the other core, and the interrupt goes out after the lock is dropped: the
 * core that takes it claims from the same queue under the same lock, and an
 * interrupt sent with the lock held would leave it spinning for the length of
 * this section. The pair of numbers - how many tasks went to another core, and
 * which core last got one - is what a port with a wake-up interrupt shows in
 * its report of the machine (docs/debts.md, K8).
 */
static k_u32_t g_wake_mask;
static k_u32_t g_remote_wakes;
static k_u32_t g_last_wake_cpu;
static k_u32_t g_last_wake_tick;

/* A full queue reported from the section, printed by the tick outside it. */
static k_u32_t g_queue_full;
static k_u8_t g_queue_full_prio;

/* --- the lock and the core of a task --- */

/*
 * The scheduler lock. It is the critical section of the port, which the
 * contract defines as the lock of the kernel bookkeeping against the other
 * core and against an interrupt; the kernel adds the count of its own nesting
 * (kernel/cpu.h). It is never held while a task sleeps, copies to a user or
 * waits for a device: every such path drops it first.
 */
static void sched_lock(void)
{
    k_cpu_lock();
}

static void sched_unlock(void)
{
    k_cpu_unlock();
}

/* --- the wake of another core --- */

/*
 * A task has just been put in the queue of `cpu`, and the caller holds the
 * lock. The core is told after the lock is dropped (k_sched_wake_flush), and
 * only when it is another core: a task of this core needs no interrupt, the
 * caller either keeps the CPU or is inside the tick that will look at the
 * queue anyway.
 *
 * The tick of the core at the moment of the hand-over is kept with the
 * number, because that is what makes the report of the port a measurement and
 * not a claim: a task that starts on the core at that same tick started before
 * the core's timer could have offered it (docs/debts.md, K8).
 */
static void wake_note_locked(k_u32_t cpu)
{
    struct k_cpu *record;

    if (cpu >= k_cpu_count() || cpu == k_cpu_id()) {
        return;
    }

    if (cpu < KERNEL_CPUS_MAX) {
        g_wake_mask |= 1u << cpu;
    }

    record = k_cpu_by_id(cpu);

    g_remote_wakes++;
    g_last_wake_cpu = cpu;
    g_last_wake_tick = (record != NULL) ? record->ticks : 0u;
}

/*
 * Send the interrupts the hand-overs asked for. The mask is taken under the
 * lock and cleared there, so two cores that wake two others in the same
 * moment each send their own and neither loses the other's.
 */
void k_sched_wake_flush(void)
{
    k_u32_t mask;
    k_u32_t cpu;

    sched_lock();
    mask = g_wake_mask;
    g_wake_mask = 0u;
    sched_unlock();

    for (cpu = 0; cpu < KERNEL_CPUS_MAX; cpu++) {
        if ((mask & (1u << cpu)) != 0u) {
            k_port_cpu_wake(cpu);
        }
    }
}

/*
 * The core that owns the queue of a task. Callers hold the lock, and the
 * field is written under it; the load is atomic because the port reads the
 * same field about itself without the lock (k_port_cpu_id, port_host.c), and
 * one side of a pair that TSan can see has to be the same kind as the other.
 */
static k_u32_t task_cpu(const struct task *t)
{
#if KERNEL_SMP
    k_u32_t cpu = __atomic_load_n(&t->cpu, __ATOMIC_RELAXED);

    return (cpu < k_cpu_count()) ? cpu : 0u;
#else
    K_UNUSED(t);
    return 0u;
#endif
}

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
    ready_queue_t *q = &g_queues[task_cpu(t)].ready[t->prio];

    /* A task must be in the queue at most once. */
    if (ready_contains(q, t)) {
        return;
    }

    if (q->count >= SCHED_READY_CAPACITY) {
        /*
         * The queue is dimensioned for every task of the system, so this can
         * only happen if a task was added twice. The task is not dropped.
         * The line waits for the tick: a sink of the console may take a
         * mutex, and this runs with the interrupts off.
         */
        (void)__atomic_add_fetch(&g_queue_full, 1, __ATOMIC_SEQ_CST);
        (void)__atomic_store_n(&g_queue_full_prio, t->prio, __ATOMIC_SEQ_CST);
        return;
    }

    q->slots[q->tail] = t;
    q->tail = (k_u8_t)((q->tail + 1) % SCHED_READY_CAPACITY);
    q->count++;
}

static void ready_remove(struct task *t)
{
    ready_queue_t *q = &g_queues[task_cpu(t)].ready[t->prio];
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

/* --- the table of the sleeping tasks (callers hold the lock) --- */

static void sleep_remove_locked(struct task *t)
{
    k_size_t i;

    for (i = 0; i < g_sleeping_count; i++) {
        if (g_sleeping[i] == t) {
            g_sleeping_count--;
            g_sleeping[i] = g_sleeping[g_sleeping_count];
            g_sleeping[g_sleeping_count] = NULL;
            return;
        }
    }
}

/*
 * The highest priority task of one core, or NULL. Callers hold the lock.
 *
 * The queue of a core holds the tasks that are ready to run there and, by the
 * design of the switch of this kernel, the task that is running there as well
 * (it leaves the queue when its quantum is over, when it sleeps or when it
 * blocks). A pick may therefore return a running task - "the caller
 * continues" - which is the `with_running` flag. The claim of a core looks at
 * the queue of that core only, so a running task is never seen by another
 * core (ADR 0012).
 */
static struct task *queue_pick_for(k_u32_t cpu, bool with_running)
{
    int prio;

    for (prio = TASK_PRIO_MAX; prio >= TASK_PRIO_MIN; prio--) {
        ready_queue_t *q = &g_queues[cpu].ready[prio];
        k_u8_t i;

        for (i = 0; i < q->count; i++) {
            struct task *t = q->slots[(q->head + i) % SCHED_READY_CAPACITY];

            if (t == NULL) {
                continue;
            }

            if (t->state == TASK_STATE_READY || (with_running && t->state == TASK_STATE_RUNNING)) {
                return t;
            }
        }
    }

    return NULL;
}

/* The number of ready tasks of one core. Callers hold the lock. */
static k_size_t queue_count(k_u32_t cpu)
{
    k_size_t count = 0;
    int prio;

    for (prio = TASK_PRIO_MIN; prio <= TASK_PRIO_MAX; prio++) {
        count += g_queues[cpu].ready[prio].count;
    }

    return count;
}

/*
 * Move a ready task to another core. This is the only way a task changes its
 * queue, and it is the scheduler that calls it: a running task is never moved
 * in the middle of its slice (ADR 0012), a blocked or sleeping one only
 * remembers the core it will be woken on.
 */
static int migrate_locked(struct task *t, k_u32_t cpu)
{
    if (t == NULL || cpu >= k_cpu_count()) {
        return -1;
    }

#if !KERNEL_SMP
    /* One core: the only migration there is stays where it is. */
    K_UNUSED(cpu);
    return 0;
#else
    if (t->state == TASK_STATE_RUNNING) {
        return -1;
    }

    if (t->state == TASK_STATE_READY) {
        ready_remove(t);
    }

    __atomic_store_n(&t->cpu, cpu, __ATOMIC_RELAXED);

    if (t->state == TASK_STATE_READY) {
        ready_enqueue(t);
        wake_note_locked(cpu);
    }

    return 0;
#endif
}

/* --- public API --- */

void k_sched_init(void)
{
    sched_lock();
    k_cpu_init();
    memset(g_queues, 0, sizeof(g_queues));
    memset(g_sleeping, 0, sizeof(g_sleeping));
    g_sleeping_count = 0;
    g_ticks = 0;
    g_next_cpu = 0;
    g_initialised = true;
    sched_unlock();
}

void k_sched_tick(void)
{
    k_size_t i = 0;
    struct task *current;
    struct k_cpu *self = k_cpu_self();
    struct task *poke[TASK_MAX_TASKS];
    k_size_t poked = 0;
    bool charge;

    sched_lock();

    /*
     * The time base of the kernel is one clock, not one per core: the tick of
     * the core 0 advances it, and every core counts the ticks it took for
     * itself. A port whose timer is on another core would have to name that
     * core here; every port of the tree keeps its clock on the core of the
     * boot (the PIT of x86-64 and the one thread of the host), so the rule is
     * the plain one: the machine has a millisecond, a core has its ticks.
     */
    if (self->id == 0u) {
        g_ticks++;
    }
    self->ticks++;

    /* Wake up the tasks whose timeout has expired. */
    while (i < g_sleeping_count) {
        struct task *t = g_sleeping[i];
        bool interrupted = task_signal_interrupts(t);

        /*
         * A signal cuts a sleep short: the task has to come back to the
         * kernel to answer it, and a wait of a second must not delay the
         * handler of a signal that arrived in the first millisecond. An
         * ignored signal and the default SIGCHLD do not wake anybody.
         */
        if ((t->wake_tick <= g_ticks || interrupted) && t->state == TASK_STATE_SLEEPING) {
            g_sleeping[i] = g_sleeping[g_sleeping_count - 1];
            g_sleeping_count--;
            t->state = t->wake_state;
            if (t->state == TASK_STATE_READY || t->state == TASK_STATE_RUNNING) {
                ready_enqueue(t);
                /* The task may belong to another core: that core is told after
                 * the lock is dropped, below. */
                wake_note_locked(task_cpu(t));

                /*
                 * A signal cuts the sleep of a port that only ends with its
                 * deadline as well: the task must not sit out the rest of a
                 * longer sleep of the port before it answers - the host
                 * sleeps in the condition of its task (ADR 0046). A deadline
                 * needs no door: the sleep of the port is as long as the
                 * deadline, and the task looks at its record when it ends.
                 */
                if (interrupted && poked < K_ARRAY_SIZE(poke)) {
                    poke[poked++] = t;
                }
            }
            continue;
        }
        i++;
    }

    /*
     * The processor time of the running task is one tick: the unit of the
     * CPU limit is the millisecond, and a tick of every port is one.
     *
     * The running task is the one of this core when the kernel has more than
     * one; with the flag off it is the task of the port, exactly as in 1.0.
     * The pointer is read under the same lock the switch writes it with, and
     * the charge itself runs outside it: it takes the lock of its own and a
     * task must never be slept on or logged with the scheduler lock held.
     */
#if KERNEL_SMP
    current = self->current;
#else
    current = k_port_task_current();
#endif

    /*
     * Only a task that is running spends processor time: the `current` of a
     * core can still name the task that left it a moment ago, and charging
     * that task for the time of its wait is the accounting of a machine of
     * one core gone wrong.
     */
    charge = current != NULL && current->state == TASK_STATE_RUNNING;

    sched_unlock();

    /*
     * The sleeps of the port that the tick ended are cut here, with the
     * scheduler lock dropped: the door of a port takes a lock of its own (the
     * host's condition), and the kernel never holds this one across a wait.
     */
    for (i = 0; i < poked; i++) {
        k_port_task_wake(poke[i]);
    }

    if (__atomic_exchange_n(&g_queue_full, 0, __ATOMIC_SEQ_CST) != 0) {
        k_u8_t prio = (k_u8_t)__atomic_load_n(&g_queue_full_prio, __ATOMIC_SEQ_CST);

        kerror("sched: the run queue of the priority %u is full", (unsigned)prio);
    }

    if (charge) {
        task_cpu_tick(current);
    }

    sched_lock();

    if (g_quantum > 0) {
#if KERNEL_SMP
        current = self->current;
#endif
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

    sched_unlock();

    /* A sleeping task of another core came back above: that core starts it
     * now instead of at its own next tick. */
    k_sched_wake_flush();
}

void k_sched_set_quantum(k_u32_t ticks)
{
    k_size_t i;

    sched_lock();
    g_quantum = (ticks < SCHED_QUANTUM_MIN) ? SCHED_QUANTUM_MIN : ticks;
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = task_get_by_index_unlocked(i);
        if (t != NULL && t->quantum_left > g_quantum) {
            t->quantum_left = g_quantum;
        }
    }
    sched_unlock();
}

k_u32_t k_sched_quantum(void)
{
    k_u32_t quantum;

    sched_lock();
    quantum = g_quantum;
    sched_unlock();

    return quantum;
}

k_u32_t k_sched_quantum_left(void)
{
    struct task *self = task_self();
    k_u32_t left = 0;

    if (self != NULL) {
        sched_lock();
        left = self->quantum_left;
        sched_unlock();
    }

    return left;
}

k_u32_t k_sched_ticks(void)
{
    k_u32_t ticks;

    sched_lock();
    ticks = g_ticks;
    sched_unlock();

    return ticks;
}

k_u32_t k_sched_cpu_ticks(k_u32_t cpu)
{
    struct k_cpu *record = k_cpu_by_id(cpu);
    k_u32_t ticks;

    if (record == NULL) {
        return 0u;
    }

    sched_lock();
    ticks = record->ticks;
    sched_unlock();

    return ticks;
}

k_u64_t k_sched_uptime_ms(void)
{
    /*
     * The real time of the board, not the number of the ticks: the timer
     * of the kernel can not keep up when the processor is busy.
     */
    return (k_u64_t)(k_port_uptime_us() / 1000u);
}

void k_sched_yield(void)
{
    struct task *self = task_self();

    if (self != NULL) {
        sched_lock();
        if (self->state == TASK_STATE_RUNNING) {
            self->state = TASK_STATE_READY;
        }
        ready_move_to_tail(self);
        sched_unlock();
    }

    k_port_task_yield();
}

void k_sched_sleep_ms(k_u32_t ms)
{
    (void)k_sched_sleep_unless(ms, NULL);
}

bool k_sched_sleep_unless(k_u32_t ms, const bool *stop)
{
    struct task *self = task_self();
    task_state_t previous;
    k_u64_t deadline_us;

    if (self == NULL) {
        /* Not running in a task context: just wait. */
        k_port_task_sleep_ms(ms);
        return true;
    }

    /* The time before the wait is processor time; the wait itself is not. */
    task_cpu_tick(self);

    sched_lock();

    /*
     * The flag is the record of a wait (kernel/wait.c) and is written under
     * this lock: a wakeup that has already happened is not slept on, which is
     * the window of K15 closed at its source.
     */
    if (stop != NULL && *stop) {
        sched_unlock();
        return false;
    }

    previous = self->state;
    self->state = TASK_STATE_SLEEPING;
    /*
     * A blocked task stays blocked, everything else returns to the run
     * queue: after the timeout the task is runnable again.
     */
    self->wake_state = (previous == TASK_STATE_BLOCKED) ? TASK_STATE_BLOCKED : TASK_STATE_READY;
    /*
     * The deadline of this sleep is a number of the clock of the port and not
     * a number of ticks: a tick is the scheduler's unit, and a port whose
     * thread of the clock runs beside the sleep (the host) can take them
     * faster or slower than the milliseconds of the caller. What the caller
     * measures, and what the kernel promises, is the clock, and it is read in
     * microseconds so that a sleep of a millisecond is a millisecond and not
     * the rest of the millisecond a tick was counted in.
     */
    deadline_us = k_port_uptime_us() + (k_u64_t)ms * 1000u;
    self->wake_tick = g_ticks + ms;
    self->cpu_stamp_ms = k_sched_uptime_ms();
    /* A sleeping task is not in the run queue. */
    ready_remove(self);
    if (g_sleeping_count < TASK_MAX_TASKS) {
        g_sleeping[g_sleeping_count++] = self;
    }
    sched_unlock();

    /*
     * The sleep of the port may end before the clock of the port has reached
     * the deadline: the door of a port is a nudge and a nudge may have lost
     * its race with the sleep it was meant for, a port rounds its timeout,
     * and a tick of the scheduler that runs faster than the clock (the host
     * under a slow runner) wakes a sleeper early. The task sleeps the rest in
     * a loop on the deadline it took on the way in, so that a sleep of `ms`
     * is never shorter than `ms`.
     *
     * Only the kernel ends a sleep early, and there are three reasons: the
     * record of a wait has been woken (`stop`, the same critical section of
     * the arm, and a signal the task has to answer, which a wait answers with
     * EINTR), a block or an unblock arrived while the task slept. The state
     * of the task is not one of them: a wake of an object that is not this
     * sleep - or a tick that fired early - puts READY there, and a sleep
     * that trusted the state would answer at once (the gate of 1.9 under
     * valgrind found a session and a terminal that did exactly that). A plain
     * sleep is not cut by a signal either: 1.8 slept it to its end, and the
     * delay of a login that a signal could shorten is a delay an attacker
     * chooses.
     */
    for (;;) {
        k_u32_t left;
        k_u64_t now_us;

        sched_lock();

        if (stop != NULL && (*stop || task_signal_interrupts(self))) {
            sched_unlock();
            break;
        }

        if (self->pending_wake || self->pending_block) {
            sched_unlock();
            break;
        }

        now_us = k_port_uptime_us();

        if (now_us >= deadline_us) {
            sched_unlock();
            break;
        }

        /* The port sleeps in milliseconds: the rest is rounded up, so that a
         * round never ends before the clock it is counting to. */
        left = (k_u32_t)((deadline_us - now_us + 999u) / 1000u);

        /*
         * The task is asleep for this round whatever a wake of another object
         * did to its state between two rounds, and the tick has the remainder
         * of this round as its deadline: the sleep of a task is one record in
         * the scheduler and one record in the loop, and the two agree.
         */
        if (self->state != TASK_STATE_SLEEPING) {
            sleep_remove_locked(self);
            ready_remove(self);
            self->state = TASK_STATE_SLEEPING;
            if (g_sleeping_count < TASK_MAX_TASKS) {
                g_sleeping[g_sleeping_count++] = self;
            }
        }

        self->wake_tick = g_ticks + left;

        sched_unlock();

        k_port_task_sleep_ms(left);
    }

    sched_lock();

    /* The wait is not processor time: the clock starts again here. */
    self->cpu_stamp_ms = k_sched_uptime_ms();

    /* The task may already have been woken up by the tick handler, or by the
     * wake of the object it waits for. */
    sleep_remove_locked(self);

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

    sched_unlock();

    return true;
}

/*
 * Wake a task that is asleep, now (kernel/wait.c, the wake of a channel).
 *
 * The task leaves the table of the sleepers and becomes what its sleep was
 * going to make it - READY for every wait, BLOCKED for a task that was
 * blocked before it slept. A READY task enters the queue of its core and the
 * core is marked in the mask of the hand-overs, which the caller sends after
 * dropping the lock; a task of this very core needs no interrupt, the caller
 * either keeps the CPU or is inside the tick that looks at the queue anyway.
 *
 * The task is not moved to another core here: the wake puts it where it was
 * going to run and the scheduler decides the rest (ADR 0012).
 */
bool k_sched_wake_task_locked(struct task *t)
{
    if (t == NULL || t->state != TASK_STATE_SLEEPING) {
        return false;
    }

    sleep_remove_locked(t);
    t->state = t->wake_state;

    if (t->state != TASK_STATE_READY && t->state != TASK_STATE_RUNNING) {
        return false;
    }

    ready_enqueue(t);
    wake_note_locked(task_cpu(t));

    return true;
}

void k_sched_block(struct task *t)
{
    if (t == NULL) {
        return;
    }

    /* The time before the block is processor time; a waiting task has none. */
    task_cpu_tick(t);

    sched_lock();

    if (t->state == TASK_STATE_SLEEPING) {
        /* The task waits with a timeout: remember the request. */
        t->pending_block = true;
        t->pending_wake = false;
    } else if (t->state == TASK_STATE_READY || t->state == TASK_STATE_RUNNING) {
        ready_remove(t);
        t->state = TASK_STATE_BLOCKED;
        t->cpu_stamp_ms = k_sched_uptime_ms();
    }

    sched_unlock();
}

void k_sched_unblock(struct task *t)
{
    bool poke = false;

    if (t == NULL) {
        return;
    }

    sched_lock();

    if (t->state == TASK_STATE_SLEEPING) {
        /* The task waits with a timeout: remember the request, and cut the
         * sleep of the port so that the request is applied now and not at
         * the deadline of a sleep it has nothing to do with. */
        t->pending_wake = true;
        t->pending_block = false;
        poke = true;
    } else if (t->state == TASK_STATE_BLOCKED) {
        t->state = TASK_STATE_READY;
        t->cpu_stamp_ms = k_sched_uptime_ms();
        ready_enqueue(t);
        wake_note_locked(task_cpu(t));
    }

    sched_unlock();

    if (poke) {
        k_port_task_wake(t);
    }

    k_sched_wake_flush();
}

/*
 * The choice of this core, under the scheduler lock.
 *
 * Only the queue of this core is looked at. A task of another queue is never
 * taken, and the reason is not a missing optimisation: this kernel keeps the
 * task that runs in the queue of its core (it leaves it when its quantum is
 * over, when it sleeps or when it blocks), and a task whose quantum has just
 * expired is READY in that queue for the few instructions between the tick
 * that moved it and the switch that leaves it. A second core that took it in
 * that window would resume a saved stack pointer whose stack is still being
 * used by the core it runs on - a task on two cores at once, the failure ADR
 * 0012 forbids.
 *
 * A task changes its core at creation (round robin), when it is woken after
 * the scheduler moved it, and by k_sched_migrate(). That is the whole of the
 * load balance, and it is honest: a core that idles beside a busy one is a
 * number the boot and /proc can show, not a race to hide.
 */
static struct task *pick_for_core_locked(k_u32_t cpu)
{
    return queue_pick_for(cpu, true);
}

/*
 * Look at the queues and answer the task this core would run next, without
 * taking it. The port of x86-64 uses k_sched_claim_next() instead: a choice
 * that is only a look is a choice another core can repeat, and one task would
 * then be running on two cores. This call is the peek that the tests and the
 * diagnostics use.
 */
struct task *k_sched_pick_next(void)
{
    struct task *next;

    sched_lock();
    next = pick_for_core_locked(k_cpu_id());
    sched_unlock();

    return next;
}

/* --- the claim: the choice and the running state in one lock --- */

/*
 * The claim of the task this core runs next (the critical section the port
 * of x86-64 switches from).
 *
 * The scheduler lock is what makes the choice and the mark one step: the
 * task leaves the ready set of its queue and becomes RUNNING on this core
 * under the same lock that chose it, so a second core cannot find it ready
 * in the window between the choice and the jump of the switch. Without this
 * call two cores could pick one task and resume one saved stack pointer -
 * the failure ADR 0012 calls a task that runs on two cores at once.
 *
 * The caller has already taken the task it leaves out of the running state:
 * a sleep, a block, a yield and an exit all change the state before they
 * reach the port, and the port calls this only after them. A task that still
 * says RUNNING stays RUNNING (and stays in the queue of its core, which is
 * how this kernel has always marked the task on the CPU); no other core can
 * take it, because the pick of a core looks at its own queue only.
 *
 * A task without a context cannot be switched to: it stays where it is and
 * NULL is returned, so that the port keeps the current task instead of
 * jumping into a zero. Returns the task that now runs on this core, or NULL
 * when nothing is runnable here.
 */
struct task *k_sched_claim_next(void)
{
    struct k_cpu *self;
    struct task *next;
    k_u32_t cpu = k_cpu_id();

    sched_lock();

    next = pick_for_core_locked(cpu);

    if (next == NULL) {
        sched_unlock();
        return NULL;
    }

    if (next->ctx == NULL) {
        sched_unlock();
        kerror("sched: the task '%s' has no context to run on", next->name);
        return NULL;
    }

    self = k_cpu_self();

    /*
     * The two writes below are the claim. The task was in the queue of this
     * core - the only one the pick looked at - so the mark and the assignment
     * of the core are the whole of the record.
     */
    if (next->state != TASK_STATE_RUNNING) {
        next->state = TASK_STATE_RUNNING;
        next->runtime_ticks++;
    }

    self->current = next;
    __atomic_store_n(&next->cpu, cpu, __ATOMIC_RELAXED);

    sched_unlock();

    return next;
}

k_size_t k_sched_ready_count(void)
{
    k_size_t count = 0;
    k_u32_t cpu;

    sched_lock();
    for (cpu = 0; cpu < k_cpu_count(); cpu++) {
        count += queue_count(cpu);
    }
    sched_unlock();

    return count;
}

k_size_t k_sched_ready_count_cpu(k_u32_t cpu)
{
    k_size_t count;

    if (k_cpu_by_id(cpu) == NULL) {
        return 0;
    }

    sched_lock();
    count = queue_count(cpu);
    sched_unlock();

    return count;
}

k_size_t k_sched_sleeping_count(void)
{
    k_size_t count;

    sched_lock();
    count = g_sleeping_count;
    sched_unlock();

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
    k_u32_t cpu;

    sched_lock();

    /* Every ready task has to be in the queue of its priority level. */
    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = task_get_by_index_unlocked(i);

        if (t == NULL) {
            continue;
        }

        if (t->state == TASK_STATE_READY &&
            !ready_contains(&g_queues[task_cpu(t)].ready[t->prio], t)) {
            kerror("sched: the task '%s' is ready, but not in the queue", t->name);
            problems++;
        }
    }

    /*
     * Every task in a queue has to be runnable there: ready, or running in
     * that queue (the switch of this kernel keeps the running task in it
     * until its quantum, a sleep or a block), and it has to be the queue of
     * its own core. A blocked, sleeping or dead task left in a queue is a
     * removal that did not happen - the bug this check exists for.
     */
    for (cpu = 0; cpu < k_cpu_count(); cpu++) {
        int prio;

        for (prio = TASK_PRIO_MIN; prio <= TASK_PRIO_MAX; prio++) {
            ready_queue_t *q = &g_queues[cpu].ready[prio];
            k_u8_t i2;

            for (i2 = 0; i2 < q->count; i2++) {
                k_u8_t idx = (k_u8_t)((q->head + i2) % SCHED_READY_CAPACITY);
                struct task *t = q->slots[idx];

                if (t == NULL) {
                    kerror("sched: the queue of the core %u, priority %d holds no task",
                           (unsigned)cpu, prio);
                    problems++;
                    continue;
                }

                if (t->state != TASK_STATE_READY && t->state != TASK_STATE_RUNNING) {
                    kerror("sched: the queue of the core %u, priority %d holds '%s' in the state "
                           "%s",
                           (unsigned)cpu, prio, t->name, task_state_name(t->state));
                    problems++;
                } else if (task_cpu(t) != cpu) {
                    kerror("sched: the queue of the core %u, priority %d holds '%s' of the core "
                           "%u",
                           (unsigned)cpu, prio, t->name, (unsigned)task_cpu(t));
                    problems++;
                }
            }
        }
    }

    /*
     * The running task of a core is the task that core last claimed, and the
     * claim of a task writes the record of the core, the state and the core
     * of the task together. Two cores that name one task as theirs is the
     * failure of a claim made without the lock - two cores would resume one
     * saved stack pointer - and it is the one property here that holds for
     * every port: the host gives a task a pthread and a logical core, so its
     * `current` is a label where the state of the task may already be the
     * next one, but a task is still the current of at most one core.
     */
    for (cpu = 0; cpu < k_cpu_count(); cpu++) {
        struct task *t = k_cpu_by_id(cpu)->current;
        k_u32_t other;

        if (t == NULL) {
            continue;
        }

        for (other = cpu + 1u; other < k_cpu_count(); other++) {
            if (k_cpu_by_id(other)->current == t) {
                kerror("sched: the task '%s' runs on the cores %u and %u", t->name, (unsigned)cpu,
                       (unsigned)other);
                problems++;
            }
        }
    }

    sched_unlock();

    return problems;
}

void k_sched_task_ready(struct task *t)
{
    if (t == NULL) {
        return;
    }

    sched_lock();

#if KERNEL_SMP
    /*
     * A task that has never run starts on the next core of the round robin;
     * one that is already known keeps the core it was put on, so a wake-up
     * does not move a task that the scheduler did not decide to move.
     */
    if (t->state == TASK_STATE_NEW) {
        __atomic_store_n(&t->cpu, g_next_cpu % k_cpu_count(), __ATOMIC_RELAXED);
        g_next_cpu = (g_next_cpu + 1u) % k_cpu_count();
    }
#endif

    t->state = TASK_STATE_READY;
    ready_enqueue(t);
    wake_note_locked(task_cpu(t));
    sched_unlock();

    k_sched_wake_flush();
}

void k_sched_task_running(struct task *t)
{
    struct k_cpu *self;

    if (t == NULL) {
        return;
    }

    sched_lock();

    self = k_cpu_self();

#if KERNEL_SMP
    /*
     * The core that runs a task is the core the task belongs to from now on;
     * the field is written here and nowhere else while the task runs.
     */
    __atomic_store_n(&t->cpu, self->id, __ATOMIC_RELAXED);
#endif

    self->current = t;
    t->state = TASK_STATE_RUNNING;
    t->runtime_ticks++;
    sched_unlock();
}

void k_sched_task_terminated(struct task *t)
{
    if (t == NULL) {
        return;
    }

    sched_lock();

    /* The task is not in the run queue any more. */
    ready_remove(t);
    sleep_remove_locked(t);

    t->state = TASK_STATE_ZOMBIE;

    if (k_cpu_self()->current == t) {
        k_cpu_self()->current = NULL;
    }

    sched_unlock();
}

int k_sched_migrate(struct task *t, k_u32_t cpu)
{
    int rc;

    sched_lock();
    rc = migrate_locked(t, cpu);
    sched_unlock();

    k_sched_wake_flush();

    return rc;
}

/*
 * The tasks the scheduler has handed to a core other than the one that ran the
 * caller, and the last of them: the core and its tick at the moment of the
 * hand-over. The port of x86-64 prints the pair beside the count of the tasks
 * it started with an interrupt (ADR 0012, docs/debts.md K8).
 */
k_u32_t k_sched_remote_wakes(void)
{
    k_u32_t count;

    sched_lock();
    count = g_remote_wakes;
    sched_unlock();

    return count;
}

void k_sched_last_remote_wake(k_u32_t *cpu, k_u32_t *tick)
{
    sched_lock();

    if (cpu != NULL) {
        *cpu = g_last_wake_cpu;
    }

    if (tick != NULL) {
        *tick = g_last_wake_tick;
    }

    sched_unlock();
}

k_u32_t k_sched_task_cpu(const struct task *t)
{
    k_u32_t cpu;

    if (t == NULL) {
        return 0u;
    }

#if KERNEL_SMP
    cpu = __atomic_load_n(&t->cpu, __ATOMIC_RELAXED);
#else
    cpu = 0u;
#endif

    return cpu;
}

k_u32_t k_sched_cpu_count(void)
{
    return k_cpu_count();
}
