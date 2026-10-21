/*
 * Micro OS — unit test runner.
 *
 * The tests run inside a kernel task: this way they exercise the very same
 * task API that the applications on the device will use.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <unistd.h>

#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_host.h"

static volatile bool g_done;
static int g_result;

static void run_tests(void *arg)
{
    K_UNUSED(arg);

    test_version();
    test_board();
    test_log();
    test_task();
    test_sched();
    test_sched_queue();
    test_kmod();
    test_kmm();
    test_mfs();
    test_mfs_persistence();
    test_kshell();
    test_boot();

    g_result = ktest_summary();
    g_done = true;
}

int main(void)
{
    struct task *runner;

    printf("Micro OS unit tests\n\n");

    k_sched_init();
    port_host_start_tick(1);

    runner = task_create("test-runner", run_tests, NULL, TASK_PRIO_DEFAULT);
    if (runner == NULL) {
        printf("cannot create the test runner task\n");
        return 1;
    }

    while (!g_done) {
        usleep(1000);
    }

    port_host_stop_tick();

    return g_result;
}
