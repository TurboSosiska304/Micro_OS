#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_host.h"

static volatile int g_blocked;

static void blocker(void *arg) {
    struct task *self = task_self();

    K_UNUSED(arg);

    if (self == NULL) {
        return;
    }

    k_sched_block(self);
    g_blocked++;

    /*
     * Stay blocked until the test unblocks us. The port level sleep is used
     * on purpose: the kernel sleep would switch the state to SLEEPING, while
     * a task waiting for an event keeps the BLOCKED state (in Linux such a
     * task would sit in a wait queue).
     */
    while (self->state == TASK_STATE_BLOCKED) {
        k_port_task_sleep_ms(1);
    }
}

static void test_ready_queue(void) {
    struct task *low = NULL;
    struct task *high = NULL;
    struct task *next;
    int waited = 0;

    ktest_begin("sched: the highest priority ready task is picked");

    port_host_set_sink(tmpfile());
    g_blocked = 0;

    low = task_create("low", blocker, NULL, 10);
    high = task_create("high", blocker, NULL, 12);

    KT_CHECK(low != NULL);
    KT_CHECK(high != NULL);

    while (g_blocked < 2 && waited < 2000) {
        k_sched_sleep_ms(1);
        waited++;
    }

    KT_CHECK_INT_EQ(g_blocked, 2);
    KT_CHECK_INT_EQ(k_sched_blocked_count(), 2);
    /* Only the test runner itself is left in the run queue. */
    KT_CHECK_INT_EQ(k_sched_ready_count(), 1);

    /* Unblocking the low priority task first makes it the next one. */
    k_sched_unblock(low);
    KT_CHECK_INT_EQ(k_sched_ready_count(), 2);
    next = k_sched_pick_next();
    KT_CHECK(next == low);

    /* The high priority task wins the next round. */
    k_sched_unblock(high);
    KT_CHECK_INT_EQ(k_sched_ready_count(), 3);
    next = k_sched_pick_next();
    KT_CHECK(next == high);

    if (low != NULL) {
        task_reap(low->pid);
    }
    if (high != NULL) {
        task_reap(high->pid);
    }

    port_host_set_sink(NULL);
}

static void test_sleep(void) {
    k_u64_t start;
    k_u64_t elapsed;

    ktest_begin("sched: sleeping blocks the task for the requested time");

    start = k_sched_uptime_ms();
    k_sched_sleep_ms(60);
    elapsed = k_sched_uptime_ms() - start;

    /* The tick source advances in 1 ms steps, allow some slack. */
    KT_CHECK(elapsed >= 50);
}

static void test_ticks(void) {
    k_u32_t before;
    k_u32_t after;

    ktest_begin("sched: the tick counter advances");

    before = k_sched_ticks();
    k_sched_sleep_ms(20);
    after = k_sched_ticks();

    KT_CHECK(after > before);
}

void test_sched(void) {
    test_ready_queue();
    test_sleep();
    test_ticks();
}
