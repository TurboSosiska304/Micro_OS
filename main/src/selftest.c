/*
 * Micro OS — on device self test.
 *
 * Exercises the task and scheduler API on the real hardware: several tasks
 * with different priorities sleep, yield and wake up, and the init task
 * checks the results. The output goes to the UART console.
 */
#include "selftest.h"

#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/task.h"

#define SELFTEST_TASKS 3
#define SELFTEST_STEPS 5
#define SELFTEST_STEP_MS 50

static volatile int s_progress[SELFTEST_TASKS];
static volatile int s_finished;
static volatile int s_context_switches;

static void selftest_worker(void *arg) {
    int id = (int)(intptr_t)arg;
    int i;

    for (i = 0; i < SELFTEST_STEPS; i++) {
        k_sched_sleep_ms(SELFTEST_STEP_MS);
        s_progress[id]++;
        s_context_switches++;
        k_sched_yield();
    }

    s_finished++;
}

static void print_task_table(void) {
    k_size_t i;
    k_size_t count = task_count();

    klog(K_LOG_LEVEL_INFO, "  pid  prio  state     name");
    klog(K_LOG_LEVEL_INFO, "  ---  ----  --------  --------");

    for (i = 0; i < count + 8; i++) {
        struct task *t = task_get_by_index(i);

        if (t == NULL) {
            continue;
        }

        klog(K_LOG_LEVEL_INFO, "  %3u  %4u  %-8s  %s", (unsigned)t->pid, (unsigned)t->prio,
             task_state_name(t->state), t->name);
    }
}

int selftest_run(void) {
    int id;
    int waited = 0;
    int failures = 0;

    klog(K_LOG_LEVEL_INFO, "selftest: starting");

    for (id = 0; id < SELFTEST_TASKS; id++) {
        char name[16];

        snprintf(name, sizeof(name), "selftest%d", id);

        if (task_create(name, selftest_worker, (void *)(intptr_t)id, (k_u8_t)(5 + id)) == NULL) {
            kerror("selftest: cannot create the task %d", id);
            return -1;
        }
    }

    while (s_finished < SELFTEST_TASKS && waited < 5000) {
        k_sched_sleep_ms(10);
        waited++;
    }

    for (id = 0; id < SELFTEST_TASKS; id++) {
        if (s_progress[id] != SELFTEST_STEPS) {
            kerror("selftest: task %d made %d of %d steps", id, s_progress[id], SELFTEST_STEPS);
            failures++;
        }
    }

    if (s_finished != SELFTEST_TASKS) {
        kerror("selftest: only %d of %d tasks finished", s_finished, SELFTEST_TASKS);
        failures++;
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
