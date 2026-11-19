#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/lock.h"
#include "kernel/sched.h"
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

void test_task(void)
{
    test_task_lifecycle();
    test_task_self();
    test_task_table();
    test_task_parent();
    test_task_wait();
    test_task_wait_timeout();
    test_mutex_creation_race();
}
