/*
 * Micro OS — on device self test.
 *
 * Exercises the task and scheduler API on the real hardware: several tasks
 * with different priorities sleep, yield and wake up, and the init task
 * checks the results. The output goes to the UART console.
 */
#include "selftest.h"

#include <stdio.h>

#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"

#define SELFTEST_TASKS 3
#define SELFTEST_STEPS 5
#define SELFTEST_STEP_MS 50

static volatile int s_progress[SELFTEST_TASKS];
static volatile int s_finished;
static volatile int s_context_switches;

/*
 * The counters are touched by several tasks at once: a counter that is
 * increased outside a critical section loses the increments that meet
 * each other there.
 */
static void counter_add(volatile int *counter)
{
    k_port_enter_critical();
    (*counter)++;
    k_port_exit_critical();
}

static void selftest_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    int i;

    for (i = 0; i < SELFTEST_STEPS; i++) {
        k_sched_sleep_ms(SELFTEST_STEP_MS);
        s_progress[id]++;
        counter_add(&s_context_switches);
        k_sched_yield();
    }

    counter_add(&s_finished);
}

static void print_task_table(void)
{
    struct task *self = task_self();
    k_size_t i;
    k_size_t count = task_count();

    klog(K_LOG_LEVEL_INFO, "selftest: %u of %u task(s) in use", (unsigned)count,
         (unsigned)TASK_MAX_TASKS);

    klog(K_LOG_LEVEL_INFO, "  pid  prio  state     name");
    klog(K_LOG_LEVEL_INFO, "  ---  ----  --------  --------");

    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = task_get_by_index(i);
        const char *state;

        if (t == NULL) {
            continue;
        }

        /* The task that prints the table is running, not just ready. */
        state = (t == self) ? "running" : task_state_name(t->state);

        klog(K_LOG_LEVEL_INFO, "  %3u  %4u  %-8s  %s", (unsigned)t->pid, (unsigned)t->prio, state,
             t->name);
    }
}

int selftest_run(void)
{
    k_u32_t pids[SELFTEST_TASKS];
    int created = 0;
    int id;
    int waited = 0;
    int failures = 0;

    klog(K_LOG_LEVEL_INFO, "selftest: starting");

    for (id = 0; id < SELFTEST_TASKS; id++) {
        char name[16];
        struct task *task;

        snprintf(name, sizeof(name), "selftest%d", id);

        task = task_create(name, selftest_worker, (void *)(intptr_t)id, (k_u8_t)(5 + id));
        if (task == NULL) {
            kerror("selftest: cannot create the task %d", id);
            failures++;
            break;
        }

        pids[id] = task->pid;
        created++;
    }

    while (s_finished < created && waited < 5000) {
        k_sched_sleep_ms(10);
        waited++;
    }

    for (id = 0; id < SELFTEST_TASKS; id++) {
        if (s_progress[id] != SELFTEST_STEPS) {
            kerror("selftest: task %d made %d of %d steps", id, s_progress[id], SELFTEST_STEPS);
            failures++;
        }
    }

    if (s_finished != created) {
        kerror("selftest: only %d of %d tasks finished", s_finished, created);
        failures++;
    }

    /*
     * Every task of the test is waited for. A task that has finished is a
     * zombie until its number is taken back, and the table of the tasks
     * has only TASK_MAX_TASKS places: a test that leaks them eats the
     * board a little on every boot.
     */
    for (id = 0; id < created; id++) {
        int code = 0;

        if (task_wait(pids[id], &code, 2000) != 0) {
            kerror("selftest: task %d did not finish", id);
            failures++;
        } else if (code != 0) {
            kerror("selftest: task %d ended with %d", id, code);
            failures++;
        }
    }

    klog(K_LOG_LEVEL_INFO, "selftest: %d task(s), %d context switch(es)", SELFTEST_TASKS,
         s_context_switches);

    print_task_table();

    if (failures == 0) {
        klog(K_LOG_LEVEL_INFO, "selftest: PASS");
    } else {
        kerror("selftest: FAIL (%d problem(s))", failures);
    }

    return failures == 0 ? 0 : -1;
}
