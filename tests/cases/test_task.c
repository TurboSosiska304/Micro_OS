#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/lock.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "port_host.h"

/* The worker runs in another task: the counters are read with the builtins. */
static int g_ran;
static int g_order[8];
static int g_order_count;

#define COUNT_GET(c) __atomic_load_n(&(c), __ATOMIC_SEQ_CST)
#define COUNT_SET(c, v) ((void)__atomic_store_n(&(c), (v), __ATOMIC_SEQ_CST))

static void worker(void *arg)
{
    int id = (int)(intptr_t)arg;

    (void)__atomic_add_fetch(&g_ran, 1, __ATOMIC_SEQ_CST);

    {
        int slot = __atomic_fetch_add(&g_order_count, 1, __ATOMIC_SEQ_CST);

        if (slot < 8) {
            g_order[slot] = id;
        }
    }
}

static void wait_for(int *counter, int value, int timeout_ms)
{
    int waited = 0;

    while (COUNT_GET(*counter) < value && waited < timeout_ms) {
        k_sched_sleep_ms(1);
        waited++;
    }
}

static void wait_for_state(k_u32_t pid, task_state_t state, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        struct task *t = task_by_pid(pid);
        if (t != NULL && task_state_of(t) == state) {
            return;
        }
        k_sched_sleep_ms(1);
        waited++;
    }
}

static void test_task_lifecycle(void)
{
    struct task *t;

    ktest_begin("task: create, run and exit");

    port_host_set_sink(tmpfile());
    COUNT_SET(g_ran, 0);

    t = task_create("worker", worker, (void *)(intptr_t)7, TASK_PRIO_DEFAULT);
    KT_CHECK(t != NULL);

    if (t != NULL) {
        KT_CHECK_STR_EQ(t->name, "worker");
        KT_CHECK(t->pid > 0);
        KT_CHECK_INT_EQ(t->prio, TASK_PRIO_DEFAULT);

        wait_for((int *)&g_ran, 1, 2000);

        KT_CHECK_INT_EQ(COUNT_GET(g_ran), 1);

        wait_for_state(t->pid, TASK_STATE_ZOMBIE, 2000);
        KT_CHECK_INT_EQ(task_count_by_state(TASK_STATE_ZOMBIE), 1);

        /* The slot can be released once the task is reaped. */
        KT_CHECK_INT_EQ(task_reap(t->pid), 0);
        KT_CHECK(task_by_pid(t->pid) == NULL);
    }

    port_host_set_sink(NULL);
}

static void test_task_self(void)
{
    struct task *self;

    ktest_begin("task: task_self() returns the running task");

    /* The test runner itself is a kernel task. */
    self = task_self();
    KT_CHECK(self != NULL);

    if (self != NULL) {
        KT_CHECK_STR_EQ(self->name, "test-runner");
        KT_CHECK(self->pid > 0);
    }

    KT_CHECK(task_by_pid(9999) == NULL);
}

static void test_task_table(void)
{
    struct task *a;
    struct task *b;

    ktest_begin("task: pids are unique and the table is bounded");

    port_host_set_sink(tmpfile());
    COUNT_SET(g_ran, 0);

    a = task_create("a", worker, (void *)(intptr_t)1, TASK_PRIO_DEFAULT);
    b = task_create("b", worker, (void *)(intptr_t)2, TASK_PRIO_DEFAULT);

    KT_CHECK(a != NULL);
    KT_CHECK(b != NULL);

    if (a != NULL && b != NULL) {
        KT_CHECK(a->pid != b->pid);
        KT_CHECK(a->pid < b->pid);
        KT_CHECK(task_by_pid(a->pid) == a);
        KT_CHECK(task_by_pid(b->pid) == b);
    }

    wait_for((int *)&g_ran, 2, 2000);
    KT_CHECK_INT_EQ(COUNT_GET(g_ran), 2);

    if (a != NULL) {
        task_reap(a->pid);
    }
    if (b != NULL) {
        task_reap(b->pid);
    }

    port_host_set_sink(NULL);
}

static void test_task_parent(void)
{
    struct task *child;
    struct task *self = task_self();

    ktest_begin("task: a new task inherits the parent");

    port_host_set_sink(tmpfile());
    COUNT_SET(g_ran, 0);

    child = task_create("child", worker, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(child != NULL);

    if (child != NULL && self != NULL) {
        KT_CHECK(child->parent == self);
    }

    wait_for((int *)&g_ran, 1, 2000);
    if (child != NULL) {
        task_reap(child->pid);
    }

    port_host_set_sink(NULL);
}

static void exiting_task(void *arg)
{
    int code = (int)(intptr_t)arg;

    k_sched_sleep_ms(10);
    task_exit(code);
}

/*
 * The child of a task that ends belongs to init from then on. A program that
 * was killed - Ctrl+C on a parent that had forked - used to leave a child
 * whose parent no longer existed: nobody called waitpid() for it and its slot
 * stayed a zombie for the life of the machine (the two-core soak found a
 * 3000-pid-old zombie of this shape). The test forks twice: a parent that
 * leaves without waiting, and its child, which must name the runner (pid 1)
 * as its parent afterwards.
 */
static volatile k_u32_t g_grandchild_pid;
static volatile int g_grandchild_ran;

static void grandchild_task(void *arg)
{
    K_UNUSED(arg);

    /* Long enough for the parent to have ended and for the test to look. */
    k_sched_sleep_ms(20);
    (void)__atomic_store_n(&g_grandchild_ran, 1, __ATOMIC_SEQ_CST);
}

static void parent_that_leaves(void *arg)
{
    struct task *child;

    K_UNUSED(arg);

    child = task_create("grandchild", grandchild_task, NULL, TASK_PRIO_DEFAULT);

    if (child != NULL) {
        (void)__atomic_store_n(&g_grandchild_pid, child->pid, __ATOMIC_SEQ_CST);
    }

    /* Returns at once, and the trampoline of the task ends it here: no wait
     * for the child it just made. */
}

static void test_task_orphan(void)
{
    struct task *parent;
    struct task *grandchild = NULL;
    struct task *self = task_self();
    k_u32_t pid = 0;
    int code = -1;
    int waited = 0;

    ktest_begin("task: a child of a task that ends goes to init");

    port_host_set_sink(tmpfile());
    (void)__atomic_store_n(&g_grandchild_pid, 0, __ATOMIC_SEQ_CST);
    (void)__atomic_store_n(&g_grandchild_ran, 0, __ATOMIC_SEQ_CST);

    parent = task_create("leaver", parent_that_leaves, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(parent != NULL);

    if (parent == NULL) {
        port_host_set_sink(NULL);
        return;
    }

    /* The parent ends without waiting for the child it made. */
    KT_CHECK_INT_EQ(task_wait(parent->pid, NULL, 2000), 0);

    pid = (k_u32_t)__atomic_load_n(&g_grandchild_pid, __ATOMIC_SEQ_CST);

    while (pid == 0u && waited < 2000) {
        k_sched_sleep_ms(1);
        waited++;
        pid = (k_u32_t)__atomic_load_n(&g_grandchild_pid, __ATOMIC_SEQ_CST);
    }

    KT_CHECK(pid != 0u);

    if (pid != 0u) {
        /* The slot is still there - the test is its reaper - and its parent
         * has to be the task of the suite, which is pid 1. */
        grandchild = task_by_pid(pid);
        KT_CHECK(grandchild != NULL);

        if (grandchild != NULL) {
            KT_CHECK(grandchild->parent == self);
            KT_CHECK_INT_EQ(task_wait(grandchild->pid, &code, 2000), 0);
            KT_CHECK_INT_EQ(code, 0);
            KT_CHECK(task_by_pid(pid) == NULL);
        }
    }

    KT_CHECK_INT_EQ(__atomic_load_n(&g_grandchild_ran, __ATOMIC_SEQ_CST), 1);

    port_host_set_sink(NULL);
}

static void test_task_wait(void)
{
    struct task *child;
    int code = -1;
    int rc;

    ktest_begin("task: the parent waits for the child");

    port_host_set_sink(tmpfile());

    child = task_create("exiting", exiting_task, (void *)(intptr_t)7, TASK_PRIO_DEFAULT);
    KT_CHECK(child != NULL);

    if (child != NULL) {
        rc = task_wait(child->pid, &code, 2000);

        KT_CHECK_INT_EQ(rc, 0);
        KT_CHECK_INT_EQ(code, 7);
        /* The task has been reaped by the wait. */
        KT_CHECK(task_by_pid(child->pid) == NULL);

        /* Waiting for a task that does not exist must fail. */
        KT_CHECK_INT_EQ(task_wait(4242, &code, 10), -1);
    }

    port_host_set_sink(NULL);
}

static void long_sleeping_task(void *arg)
{
    K_UNUSED(arg);

    /* Stays alive longer than the timeout of the test. */
    k_sched_sleep_ms(500);
}

static void test_task_wait_timeout(void)
{
    struct task *child;
    int code = -1;

    ktest_begin("task: waiting for a running task times out");

    port_host_set_sink(tmpfile());

    child = task_create("sleeper", long_sleeping_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(child != NULL);

    if (child != NULL) {
        KT_CHECK_INT_EQ(task_wait(child->pid, &code, 20), -2);
        task_reap(child->pid);
    }

    port_host_set_sink(NULL);
}

/*
 * The mutex of the kernel makes its handle on the first lock. Two tasks
 * that take a fresh mutex at the same moment are the race that creation
 * can lose: both build a handle, one wins, and the other has to be given
 * back - and only one task may be inside the lock.
 */
static k_mutex_t s_fresh;
static int s_inside;
static int s_doubles;
static int s_locks;

static void contending_task(void *arg)
{
    int i;

    (void)arg;

    for (i = 0; i < 200; i++) {
        if (k_mutex_lock(&s_fresh) != 0) {
            continue;
        }

        if (__atomic_exchange_n(&s_inside, 1, __ATOMIC_SEQ_CST) != 0) {
            (void)__atomic_add_fetch(&s_doubles, 1, __ATOMIC_SEQ_CST);
        }

        /* A point where the scheduler may run the other task. */
        k_sched_yield();

        (void)__atomic_store_n(&s_inside, 0, __ATOMIC_SEQ_CST);
        (void)__atomic_add_fetch(&s_locks, 1, __ATOMIC_SEQ_CST);
        (void)k_mutex_unlock(&s_fresh);
    }
}

/* A task of a job that is still alive when the signal of the job arrives. */
static void job_worker(void *arg)
{
    K_UNUSED(arg);

    k_sched_sleep_ms(50);
}

/*
 * The job of a pipeline (ADR 0017): the child of task_fork_shared() gets the
 * job of its parent, a signal of the job reaches every task of it, and the
 * caller that reads the console for the chain is not a target.
 */
static void test_task_job(void)
{
    struct task *self = task_self();
    struct task *child;
    k_u32_t previous;
    k_u32_t job;
    int code = -1;

    ktest_begin("task: a job is inherited and a signal reaches the whole job");

    port_host_set_sink(tmpfile());

    previous = task_get_job(self);
    job = self->pid;

    task_set_job(self, job);
    child = task_fork_shared(self, "job-child", job_worker, NULL);
    task_set_job(self, previous);

    KT_CHECK(child != NULL);

    if (child != NULL) {
        KT_CHECK_INT_EQ(task_get_job(child), job);
        /* The caller is the owner of the job, so only the child is named. */
        KT_CHECK_INT_EQ(task_signal_job(job, K_SIGINT), 0);
        KT_CHECK(task_signal_pending(child) != 0);
        KT_CHECK_INT_EQ(task_wait(child->pid, &code, 2000), 0);
    }

    KT_CHECK_INT_EQ(task_signal_job(TASK_JOB_NONE, K_SIGINT), KERR_INVAL);
    KT_CHECK_INT_EQ(task_get_job(self), previous);

    port_host_set_sink(NULL);
}

static void test_mutex_creation_race(void)
{
    struct task *a;
    struct task *b;
    int waited = 0;

    ktest_begin("mutex: two tasks create and take the same fresh lock");

    s_inside = 0;
    s_doubles = 0;
    s_locks = 0;

    a = task_create("mutex-a", contending_task, NULL, TASK_PRIO_DEFAULT);
    b = task_create("mutex-b", contending_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(a != NULL && b != NULL);

    while (__atomic_load_n(&s_locks, __ATOMIC_SEQ_CST) < 400 && waited < 5000) {
        k_sched_sleep_ms(1);
        waited++;
    }

    KT_CHECK_INT_EQ(__atomic_load_n(&s_locks, __ATOMIC_SEQ_CST), 400);
    KT_CHECK_INT_EQ(__atomic_load_n(&s_doubles, __ATOMIC_SEQ_CST), 0);

    if (a != NULL) {
        KT_CHECK_INT_EQ(task_wait(a->pid, NULL, 1000), 0);
    }
    if (b != NULL) {
        KT_CHECK_INT_EQ(task_wait(b->pid, NULL, 1000), 0);
    }

    KT_CHECK_INT_EQ(k_mutex_destroy(&s_fresh), 0);
}

/*
 * The limits of a fresh task are the ceiling of the system, and the get/set
 * of the kernel answer the same policy as the calls of the ABI: lower only,
 * EPERM for a raise, EINVAL for a resource that is not one of the three.
 */
static int g_task_limit_rc[8];
static k_u64_t g_task_limit_value[3];

static void limits_task(void *arg)
{
    struct task *self = task_self();

    K_UNUSED(arg);

    g_task_limit_value[0] = self->limit_memory;
    g_task_limit_value[1] = self->limit_cpu_ms;
    g_task_limit_value[2] = self->limit_fds;

    g_task_limit_rc[0] = task_limit_get(self, KSYS_RLIMIT_CPU, &g_task_limit_value[1]);
    g_task_limit_rc[1] = task_limit_set(self, KSYS_RLIMIT_CPU, 10);
    g_task_limit_value[1] = self->limit_cpu_ms;
    g_task_limit_rc[2] = task_limit_set(self, KSYS_RLIMIT_CPU, 100);
    g_task_limit_rc[3] = task_limit_get(self, 0, &g_task_limit_value[1]);
    g_task_limit_rc[4] = task_limit_set(self, KSYS_RLIMIT_MEMORY, 1024);
    g_task_limit_rc[5] = task_limit_set(self, KSYS_RLIMIT_FDS, TASK_MAX_FDS - 1);
    g_task_limit_rc[6] = task_fd_limit(self);
    g_task_limit_rc[7] = task_limit_set(NULL, KSYS_RLIMIT_FDS, 0);
}

static void test_task_limits(void)
{
    struct task *worker;

    ktest_begin("task: the limits start at the ceiling and only go down");

    port_host_set_sink(tmpfile());

    worker = task_create("limits", limits_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(worker != NULL);

    if (worker != NULL) {
        KT_CHECK_INT_EQ(task_wait(worker->pid, NULL, 2000), 0);

        KT_CHECK_INT_EQ((int)g_task_limit_value[0], (int)TASK_LIMIT_MEMORY_MAX);
        KT_CHECK_INT_EQ((int)g_task_limit_value[2], TASK_MAX_FDS);
        KT_CHECK_INT_EQ(g_task_limit_rc[0], 0);
        KT_CHECK_INT_EQ((int)g_task_limit_value[1], 10);
        KT_CHECK_INT_EQ(g_task_limit_rc[1], 0);
        KT_CHECK_INT_EQ(g_task_limit_rc[2], KERR_PERM);
        KT_CHECK_INT_EQ(g_task_limit_rc[3], KERR_INVAL);
        KT_CHECK_INT_EQ(g_task_limit_rc[4], 0);
        KT_CHECK_INT_EQ(g_task_limit_rc[5], 0);
        KT_CHECK_INT_EQ(g_task_limit_rc[6], TASK_MAX_FDS - 1);
        KT_CHECK_INT_EQ(g_task_limit_rc[7], KERR_INVAL);
    }

    port_host_set_sink(NULL);
}

void test_task(void)
{
    test_task_lifecycle();
    test_task_self();
    test_task_table();
    test_task_parent();
    test_task_orphan();
    test_task_wait();
    test_task_wait_timeout();
    test_task_job();
    test_mutex_creation_race();
    test_task_limits();
}
