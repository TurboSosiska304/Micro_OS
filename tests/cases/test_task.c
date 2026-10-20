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

static void worker(void *arg) {
    int id = (int)(intptr_t)arg;

    g_ran++;
    if (g_order_count < 8) {
        g_order[g_order_count++] = id;
    }
}

static void wait_for(int *counter, int value, int timeout_ms) {
    int waited = 0;

    while (*counter < value && waited < timeout_ms) {
        k_sched_sleep_ms(1);
        waited++;
    }
}

static void wait_for_state(k_u32_t pid, task_state_t state, int timeout_ms) {
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

static void test_task_lifecycle(void) {
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

static void test_task_self(void) {
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

static void test_task_table(void) {
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

void test_task(void) {
    test_task_lifecycle();
    test_task_self();
    test_task_table();
}
