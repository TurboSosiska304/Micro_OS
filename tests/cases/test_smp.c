/*
 * The kernel of two cores (ADR 0012) over the host port.
 *
 * This file is compiled into every test binary and does nothing unless the
 * build has KERNEL_SMP on: the one-core kernel has its own tests, and they
 * are the tests of 1.0. The build of `make test-smp` turns the flag on and
 * runs the whole suite on two cores (MICRO_OS_CPUS=2, tests/CMakeLists.txt).
 *
 * The host port gives every task a pthread, so the tasks here really run at
 * the same time; what the tests check is the kernel's side of the promise:
 * a task belongs to the queue of one core and nowhere else, the scheduler is
 * the only writer of that field, a running task is never moved, and a wake-up
 * of a task that sleeps is not lost.
 *
 * The number of cores comes from MICRO_OS_CPUS; the test property of
 * tests/CMakeLists.txt sets it to 2 for the `unit` test of a KERNEL_SMP build.
 * A binary of that build started without the variable is one core, and the
 * first check says so instead of passing quietly: the gate is a run of two.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdatomic.h>

#include <stdio.h>
#include <string.h>

#include "kernel/cpu.h"
#include "kernel/log.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_host.h"

#if KERNEL_SMP

#define SMP_WORKERS 6

static atomic_int g_blocked;
static atomic_int g_release;
static atomic_int g_finished;
static atomic_int g_wrong_cpu;

static struct task *g_workers[SMP_WORKERS];

static void reset_counters(void)
{
    atomic_store(&g_blocked, 0);
    atomic_store(&g_release, 0);
    atomic_store(&g_finished, 0);
    atomic_store(&g_wrong_cpu, 0);
}

/*
 * A worker that blocks at once and then samples its own core on every turn:
 * the record the scheduler keeps and the answer of the port have to agree,
 * and neither may change while the task runs (a change would be a task that
 * moved to another core in the middle of its slice).
 */
static void smp_worker(void *arg)
{
    struct task *self = task_self();
    bool stable = arg != NULL;
    k_u32_t own;

    if (self == NULL) {
        return;
    }

    k_sched_block(self);
    own = k_sched_task_cpu(self);
    (void)atomic_fetch_add(&g_blocked, 1);

    while (!atomic_load(&g_release)) {
        /*
         * The port answers the core of the task; the scheduler keeps the
         * record. A task on two cores at once - the one thing the gate of
         * ADR 0012 forbids - would show up as the two numbers disagreeing.
         * A worker of the stable kind (arg != NULL) also checks that the
         * record did not change under it: the test of the migration is the
         * one case where the scheduler moves a task on purpose, so its
         * worker is the other kind.
         */
        k_u32_t now = k_cpu_id();

        if (now != k_sched_task_cpu(self) || (stable && now != own)) {
            (void)atomic_fetch_add(&g_wrong_cpu, 1);
        }

        k_port_task_sleep_ms(1);
    }

    (void)atomic_fetch_add(&g_finished, 1);
}

/*
 * A worker that only waits: the test of the migration moves it on purpose, so
 * it must not sample its own core while the move is in the air (the move is
 * one write, and the sample of a task of the host is two reads).
 */
static void smp_idle_worker(void *arg)
{
    struct task *self = task_self();

    K_UNUSED(arg);

    if (self == NULL) {
        return;
    }

    k_sched_block(self);
    (void)atomic_fetch_add(&g_blocked, 1);

    while (!atomic_load(&g_release)) {
        k_port_task_sleep_ms(1);
    }

    (void)atomic_fetch_add(&g_finished, 1);
}

static int wait_for(atomic_int *value, int wanted, int milliseconds)
{
    int waited = 0;

    while (atomic_load(value) < wanted && waited < milliseconds) {
        k_sched_sleep_ms(1);
        waited++;
    }

    return atomic_load(value);
}

/* The task is no longer waiting for its event: the unblock arrived. */
static bool wait_unblocked(struct task *t, int milliseconds)
{
    int waited = 0;

    while (task_state_of(t) == TASK_STATE_BLOCKED && waited < milliseconds) {
        k_sched_sleep_ms(1);
        waited++;
    }

    return task_state_of(t) != TASK_STATE_BLOCKED;
}

static void workers_stop(void)
{
    int i;

    atomic_store(&g_release, 1);

    for (i = 0; i < SMP_WORKERS; i++) {
        if (g_workers[i] != NULL) {
            (void)task_wait(g_workers[i]->pid, NULL, 3000);
            g_workers[i] = NULL;
        }
    }
}

static void test_smp_cores(void)
{
    ktest_begin("smp: the kernel has the cores the port started");

    KT_CHECK_INT_EQ(k_sched_cpu_count(), 2);
    KT_CHECK_INT_EQ(k_cpu_count(), k_sched_cpu_count());
    KT_CHECK(k_cpu_by_id(0) != NULL);
    KT_CHECK(k_cpu_by_id(1) != NULL);
    KT_CHECK(k_cpu_by_id(2) == NULL);
    KT_CHECK(k_cpu_id() < k_sched_cpu_count());

    /* The host has one clock for the machine: the core 0 takes its ticks and
     * the others have none until a port gives them a timer of their own. */
    {
        k_u32_t before = k_sched_cpu_ticks(0);

        k_sched_sleep_ms(10);

        KT_CHECK(k_sched_cpu_ticks(0) > before);
    }
}

static void test_smp_spread(void)
{
    bool seen[KERNEL_CPUS_MAX];
    int i;

    ktest_begin("smp: a new task is put on the queue of both cores");

    reset_counters();
    port_host_set_sink(tmpfile());

    for (i = 0; i < SMP_WORKERS; i++) {
        char name[8];

        (void)snprintf(name, sizeof(name), "smp%d", i);
        g_workers[i] = task_create(name, smp_worker, (void *)1, TASK_PRIO_DEFAULT);
        KT_CHECK(g_workers[i] != NULL);
    }

    KT_CHECK_INT_EQ(wait_for(&g_blocked, SMP_WORKERS, 3000), SMP_WORKERS);

    memset(seen, 0, sizeof(seen));

    for (i = 0; i < SMP_WORKERS; i++) {
        if (g_workers[i] != NULL) {
            k_u32_t cpu = k_sched_task_cpu(g_workers[i]);

            KT_CHECK(cpu < k_sched_cpu_count());
            seen[cpu] = true;
            KT_CHECK_INT_EQ(task_state_of(g_workers[i]), TASK_STATE_BLOCKED);
        }
    }

    KT_CHECK(seen[0]);
    KT_CHECK(seen[1]);

    /* Every record is in the queue of its own core and nowhere else. */
    KT_CHECK_INT_EQ(k_sched_validate(), 0);

    workers_stop();

    KT_CHECK_INT_EQ(wait_for(&g_finished, SMP_WORKERS, 3000), SMP_WORKERS);
    KT_CHECK_INT_EQ(atomic_load(&g_wrong_cpu), 0);
    KT_CHECK_INT_EQ(k_sched_validate(), 0);

    port_host_set_sink(NULL);
}

static void test_smp_migration(void)
{
    struct task *t = NULL;
    k_u32_t from;
    k_u32_t to;
    int i;

    ktest_begin("smp: only the scheduler moves a task between the cores");

    reset_counters();
    port_host_set_sink(tmpfile());

    g_workers[0] = task_create("mover", smp_idle_worker, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(g_workers[0] != NULL);

    if (g_workers[0] != NULL) {
        KT_CHECK_INT_EQ(wait_for(&g_blocked, 1, 2000), 1);
        t = g_workers[0];
        from = k_sched_task_cpu(t);
        to = (from + 1u) % k_sched_cpu_count();

        /* A blocked task only remembers the core it will be woken on. */
        KT_CHECK_INT_EQ(k_sched_migrate(t, from), 0);
        KT_CHECK_INT_EQ(k_sched_task_cpu(t), from);
        KT_CHECK_INT_EQ(k_sched_migrate(t, to), 0);
        KT_CHECK_INT_EQ(k_sched_task_cpu(t), to);

        /* A core that is not there is refused; a task that has no record
         * too. (The host has no preemption point of its own, so the caller
         * is READY here and not RUNNING: the refusal of a running task is
         * the x86 path, see k_sched_migrate() and ADR 0012.) */
        KT_CHECK_INT_EQ(k_sched_migrate(t, k_sched_cpu_count()), -1);
        KT_CHECK_INT_EQ(k_sched_migrate(NULL, to), -1);

        /* The wake-up takes the queue of the core the scheduler chose. */
        k_u32_t queued = k_sched_ready_count_cpu(to);

        k_sched_unblock(t);
        KT_CHECK_INT_EQ(k_sched_task_cpu(t), to);
        KT_CHECK_INT_EQ(k_sched_ready_count_cpu(to), queued + 1);
        KT_CHECK_INT_EQ(k_sched_validate(), 0);
    }

    workers_stop();

    for (i = 0; i < SMP_WORKERS; i++) {
        g_workers[i] = NULL;
    }

    KT_CHECK_INT_EQ(atomic_load(&g_finished), 1);
    KT_CHECK_INT_EQ(atomic_load(&g_wrong_cpu), 0);
    KT_CHECK_INT_EQ(k_sched_validate(), 0);

    port_host_set_sink(NULL);
}

static void test_smp_wakeup(void)
{
    int i;

    ktest_begin("smp: a wake-up is not lost while the task sleeps");

    reset_counters();
    port_host_set_sink(tmpfile());

    for (i = 0; i < SMP_WORKERS; i++) {
        char name[8];

        (void)snprintf(name, sizeof(name), "wk%d", i);
        g_workers[i] = task_create(name, smp_worker, (void *)1, TASK_PRIO_DEFAULT);
        KT_CHECK(g_workers[i] != NULL);
    }

    KT_CHECK_INT_EQ(wait_for(&g_blocked, SMP_WORKERS, 3000), SMP_WORKERS);

    /*
     * Every task is unblocked once while the others keep the machine busy:
     * one lost wake-up leaves a worker blocked, and the wait below says so.
     */
    for (i = 0; i < SMP_WORKERS; i++) {
        if (g_workers[i] != NULL) {
            k_sched_unblock(g_workers[i]);
        }
    }

    for (i = 0; i < SMP_WORKERS; i++) {
        if (g_workers[i] != NULL) {
            KT_CHECK(wait_unblocked(g_workers[i], 3000));
        }
    }

    /* The tick of the machine keeps the queues honest while they move. */
    for (i = 0; i < 50; i++) {
        KT_CHECK_INT_EQ(k_sched_validate(), 0);
        k_sched_sleep_ms(1);
    }

    workers_stop();

    KT_CHECK_INT_EQ(wait_for(&g_finished, SMP_WORKERS, 3000), SMP_WORKERS);
    KT_CHECK_INT_EQ(atomic_load(&g_wrong_cpu), 0);

    port_host_set_sink(NULL);
}

void test_smp(void)
{
    test_smp_cores();
    test_smp_spread();
    test_smp_migration();
    test_smp_wakeup();
}

#else

void test_smp(void)
{
    /* The kernel of 1.0: one core, and the tests of the one-core scheduler
     * (test_sched.c, test_sched_queue.c) are the whole of it. */
}

#endif /* KERNEL_SMP */
