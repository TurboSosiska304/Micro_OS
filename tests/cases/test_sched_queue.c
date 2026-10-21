#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/log.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_host.h"

/*
 * The number of tasks of the test. It is deliberately bigger than the old
 * size of the run queue (8): with a smaller queue the tasks were silently
 * dropped, they stayed "ready" but were never picked up again.
 */
#define MANY_TASKS 20

/* The priority of the test tasks, the test runner has a lower one. */
#define TEST_PRIO 7

static volatile int g_blocked;
static volatile int g_release;
static struct task *g_tasks[MANY_TASKS];

static void blocker(void *arg)
{
    struct task *self = task_self();

    K_UNUSED(arg);

    if (self == NULL) {
        return;
    }

    k_sched_block(self);
    g_blocked++;

    /*
     * Wait without touching the run queue and without reacting to the state
     * of the task: the port level sleep does not change the state, so the
     * queue keeps exactly the tasks that the test put into it. The task
     * must not finish when it is unblocked, otherwise it would leave the
     * queue on its own.
     */
    while (!g_release) {
        k_port_task_sleep_ms(1);
    }
}

void test_sched_queue(void)
{
    bool seen[MANY_TASKS];
    int i;
    int waited = 0;
    int picked = 0;

    ktest_begin("sched: the run queue holds every task of the system");

    port_host_set_sink(tmpfile());
    g_blocked = 0;
    g_release = 0;

    for (i = 0; i < MANY_TASKS; i++) {
        char name[8];

        snprintf(name, sizeof(name), "q%d", i);
        g_tasks[i] = task_create(name, blocker, (void *)(intptr_t)i, TEST_PRIO);
        KT_CHECK(g_tasks[i] != NULL);
    }

    while (g_blocked < MANY_TASKS && waited < 4000) {
        k_sched_sleep_ms(1);
        waited++;
    }

    KT_CHECK_INT_EQ(g_blocked, MANY_TASKS);

    /* All the tasks are blocked, so only the test runner is in the queue. */
    KT_CHECK_INT_EQ(k_sched_ready_count(), 1);
    KT_CHECK_INT_EQ(k_sched_validate(), 0);

    /* Now all of them become ready at the same time. */
    for (i = 0; i < MANY_TASKS; i++) {
        if (g_tasks[i] != NULL) {
            k_sched_unblock(g_tasks[i]);
        }
    }

    /* The queue of the priority level has to hold all of them. */
    KT_CHECK_INT_EQ(k_sched_ready_count(), MANY_TASKS + 1);
    KT_CHECK_INT_EQ(k_sched_validate(), 0);

    ktest_begin("sched: no task is lost in the run queue");

    /*
     * Take the head of the queue and remove it, as long as there are tasks
     * of the test priority level. Every task has to appear exactly once:
     * a dropped task would be missing here.
     */
    memset(seen, 0, sizeof(seen));

    for (i = 0; i < MANY_TASKS; i++) {
        struct task *next = k_sched_pick_next();
        int j;
        int found = -1;

        if (next == NULL) {
            break;
        }

        for (j = 0; j < MANY_TASKS; j++) {
            if (g_tasks[j] == next) {
                found = j;
                break;
            }
        }

        KT_CHECK(found >= 0);
        if (found < 0) {
            break;
        }

        KT_CHECK(!seen[found]);
        seen[found] = true;
        picked++;

        /* Removing the task from the queue frees its place. */
        k_sched_block(next);
    }

    KT_CHECK_INT_EQ(picked, MANY_TASKS);
    KT_CHECK_INT_EQ(k_sched_validate(), 0);

    /* Let the tasks finish and free their slots for the next tests. */
    g_release = 1;
    k_sched_sleep_ms(50);

    port_host_set_sink(NULL);
}
