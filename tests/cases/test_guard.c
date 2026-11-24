/*
 * Micro OS — the guard page under the stack of a task (ADR 0048).
 *
 * The machine of the tests is a process of Linux, and a task of its kernel is
 * a pthread with a page under its stack that is not mapped. A recursion that
 * runs past the end of the stack does not stop the process: the port
 * recognizes the address of the guard and ends that task, the way the fault
 * of a program of ring 3 ends the program and not the machine. The test is
 * the contract of the port: the task is gone, its status is 255, and a task
 * created after it still runs.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_host.h"

/*
 * Valgrind counts the intentional store into the guard page as an invalid
 * write, and `--error-exitcode=42` would fail the suite of `make
 * test-valgrind` for the one access the test is made of. The client request
 * of the header is the documented way to ask whether the machine is the
 * instrumented one; the environment is the fallback for a build without the
 * header. The overflow itself is run by the plain and the address sanitizer
 * builds, and the rest of the port - the bounds of the stack, the alternate
 * stack, the handler and its cleanup - runs here either way.
 */
#if defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define KTEST_HAVE_VALGRIND 1
#endif
#endif

static __attribute__((unused)) bool guard_runs_here(void)
{
#if defined(KTEST_HAVE_VALGRIND)
    if (RUNNING_ON_VALGRIND) {
        return false;
    }
#endif

    {
        const char *preload = getenv("LD_PRELOAD");

        if (preload != NULL && strstr(preload, "vgpreload") != NULL) {
            return false;
        }
    }

    return true;
}

/*
 * The frame of the recursion is large enough that the stack is gone in fewer
 * than a hundred calls, and the sum after the call keeps the compiler from
 * turning the recursion into a loop (a loop never moves the stack pointer).
 */
#define GUARD_FRAME_BYTES 512u

static volatile k_u32_t g_guard_sink;
static int g_after_ran;

static __attribute__((unused)) void guard_alive_task(void *arg)
{
    K_UNUSED(arg);

    (void)__atomic_add_fetch(&g_after_ran, 1, __ATOMIC_SEQ_CST);
}

static __attribute__((noinline)) void guard_overrun(k_u32_t depth)
{
    volatile k_u8_t pad[GUARD_FRAME_BYTES];

    pad[0] = (k_u8_t)(depth & 0xFFu);
    pad[GUARD_FRAME_BYTES - 1u] = (k_u8_t)((depth >> 8) & 0xFFu);

    if (depth < 100000u) {
        guard_overrun(depth + 1u);
    }

    g_guard_sink = g_guard_sink + pad[0];
}

static __attribute__((unused)) void guard_overrun_task(void *arg)
{
    K_UNUSED(arg);

    guard_overrun(0);

    kwarn("the stack of the test task held the whole recursion");
}

void test_guard(void)
{
    ktest_begin("task: a stack overflow kills the task and not the machine");

#if defined(__SANITIZE_THREAD__)
    /*
     * The thread sanitizer intercepts siglongjmp() and looks for the buffer
     * of the call in the frames of the stack that the signal interrupted.
     * The stack is gone by definition, and the interceptor stops the process
     * before the port can end the task. The contract is the same code in the
     * plain build and in the address sanitizer, both gates of `make ci`, so
     * the thread sanitizer checks the rest of the suite and this case says
     * why it is not its case.
     */
    KT_CHECK(1);
#else
    struct task *overrun;
    struct task *alive;

    if (!guard_runs_here()) {
        /* The store into the guard is an invalid write to the instrumented
         * machine by definition (see above); the code around it has already
         * run in every task of this suite. */
        KT_CHECK(1);
        return;
    }

    port_host_set_sink(tmpfile());

    g_after_ran = 0;
    overrun = task_create("guard-overrun", guard_overrun_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(overrun != NULL);

    if (overrun != NULL) {
        k_u32_t pid = overrun->pid;
        int status = -1;

        /* The pid is read before the wait: the reaping zeroes the slot. */
        KT_CHECK_INT_EQ(task_wait(pid, &status, 10000), 0);
        KT_CHECK_INT_EQ(status, 255);

        /*
         * The machine is alive: another task is created, runs and is waited
         * for after the one that overflowed. A port that took the process
         * down with the stack would not reach this line at all.
         */
        alive = task_create("guard-alive", guard_alive_task, NULL, TASK_PRIO_DEFAULT);
        KT_CHECK(alive != NULL);

        if (alive != NULL) {
            k_u32_t alive_pid = alive->pid;
            int alive_status = -1;

            KT_CHECK_INT_EQ(task_wait(alive_pid, &alive_status, 5000), 0);
            KT_CHECK_INT_EQ(alive_status, 0);
            KT_CHECK_INT_EQ(__atomic_load_n(&g_after_ran, __ATOMIC_SEQ_CST), 1);
        }
    }
#endif
}
