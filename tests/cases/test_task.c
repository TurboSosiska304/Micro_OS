#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_host.h"

static volatile int g_ran;
static volatile int g_order[8];
static volatile int g_order_count;

static void worker(void *arg)
{
    int id = (int)(intptr_t)arg;

    g_ran++;
    if (g_order_count < 8) {
        g_order[g_order_count++] = id;
    }
}

static void wait_for(int *counter, int value, int timeout_ms)
{
    int waited = 0;

    while (*counter < value && waited < timeout_ms) {
        k_sched_sleep_ms(1);
        waited++;
    }
}

static void wait_for_state(k_u32_t pid, task_state_t state, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        struct task *t = task_by_pid(pid);
        if (t != NULL && t->state == state) {
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
    g_ran = 0;

    t = task_create("worker", worker, (void *)(intptr_t)7, TASK_PRIO_DEFAULT);
    KT_CHECK(t != NULL);

    if (t != NULL) {
        KT_CHECK_STR_EQ(t->name, "worker");
        KT_CHECK(t->pid > 0);
        KT_CHECK_INT_EQ(t->prio, TASK_PRIO_DEFAULT);

        wait_for((int *)&g_ran, 1, 2000);

        KT_CHECK_INT_EQ(g_ran, 1);

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
    g_ran = 0;

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
    KT_CHECK_INT_EQ(g_ran, 2);

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
    g_ran = 0;

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

static void test_task_fds(void)
{
    struct task *self = task_self();

    ktest_begin("task: file descriptors and the working directory");

    if (self == NULL) {
        return;
    }

    /* Every task starts with closed descriptors and the root directory. */
    KT_CHECK_INT_EQ(task_fd_get(self, 0), -1);
    KT_CHECK_STR_EQ(task_get_cwd(self), "/");

    KT_CHECK_INT_EQ(task_fd_alloc(self), 0);
    KT_CHECK_INT_EQ(task_fd_alloc(self), 1);
    KT_CHECK_INT_EQ(task_fd_get(self, 0), 0);
    KT_CHECK_INT_EQ(task_fd_get(self, 1), 1);

    KT_CHECK_INT_EQ(task_fd_set(self, 1, 42), 0);
    KT_CHECK_INT_EQ(task_fd_get(self, 1), 42);

    task_fd_close(self, 1);
    KT_CHECK_INT_EQ(task_fd_get(self, 1), -1);

    task_set_cwd(self, "/tmp");
    KT_CHECK_STR_EQ(task_get_cwd(self), "/tmp");
    task_set_cwd(self, "/");
}

void test_task(void)
{
    test_task_lifecycle();
    test_task_self();
    test_task_table();
    test_task_parent();
    test_task_wait();
    test_task_wait_timeout();
    test_task_fds();
}
