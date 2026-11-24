/*
 * Micro OS — the waits of the kernel (kernel/wait.h).
 *
 * The two shapes the wake of 1.9 has to have, and the two numbers it has to
 * show: a wake is not lost when it arrives between the look at an object and
 * the sleep of the waiter (docs/debts.md, K15), and it wakes the waiter at
 * once instead of at the next tick of the timer.
 *
 * The first case is the one of the debt, arranged on purpose: the waiter
 * hands the CPU back between the arm and the sleep, which is the window in
 * which 1.8 had no record of it and the wakeup found nothing. The second is a
 * pipe and two tasks, which is the shape `lat_pipe` of `make bench` measures
 * on x86-64: the number printed here is the same row of the host port.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "kernel/kfile.h"
#include "kernel/port.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/wait.h"
#include "port_host.h"

/* A channel of the tests: any address whose lifetime is the run. */
static const char g_test_channel;

/* The boundary of the two latencies below, in microseconds. A wake that waits
 * for a tick is a thousand; a wake that waits for a chunk is ten thousand. */
#define TEST_WAKE_LIMIT_US 5000u

/* --- the bookkeeping --- */

static void test_wait_arm(void)
{
    ktest_begin("wait: a record is armed and dropped");

    KT_CHECK_INT_EQ(k_wait_total(), 0);
    KT_CHECK(k_wait_arm(K_WAIT_CHANNEL_PIPES, NULL));
    KT_CHECK_INT_EQ(k_wait_count(K_WAIT_CHANNEL_PIPES), 1);
    KT_CHECK_INT_EQ(k_wait_total(), 1);
    KT_CHECK_INT_EQ(k_wait_sleep(0), K_WAIT_TIMEOUT);
    k_wait_disarm();
    KT_CHECK_INT_EQ(k_wait_total(), 0);
    KT_CHECK_INT_EQ(k_wait_count(K_WAIT_CHANNEL_PIPES), 0);
}

/* --- the window between the look and the sleep --- */

static atomic_int g_window_armed;
static atomic_int g_window_result;

static void window_waiter(void *arg)
{
    K_UNUSED(arg);

    (void)k_wait_arm(K_WAIT_CHANNEL_CONSOLE, NULL);
    (void)__atomic_store_n(&g_window_armed, 1, __ATOMIC_SEQ_CST);

    /*
     * The window of K15, opened on purpose: the record is armed, the task
     * gives the CPU back for a while, and the other task wakes the channel in
     * that moment - after the arm and before the sleep. A wait that lost the
     * wakeup would sit here for the whole timeout of the k_wait_sleep below.
     */
    k_sched_sleep_ms(40);

    (void)__atomic_store_n(&g_window_result, k_wait_sleep(2000), __ATOMIC_SEQ_CST);
    k_wait_disarm();
}

static void test_wait_window(void)
{
    struct task *waiter;
    k_u64_t start;
    k_u64_t spent_ms;
    int waited = 0;

    ktest_begin("wait: a wake between the arm and the sleep is not lost");

    (void)__atomic_store_n(&g_window_armed, 0, __ATOMIC_SEQ_CST);
    (void)__atomic_store_n(&g_window_result, 0x7FFFFFF, __ATOMIC_SEQ_CST);

    waiter = task_create("wt-window", window_waiter, NULL, TASK_PRIO_DEFAULT + 1);
    KT_CHECK(waiter != NULL);

    if (waiter == NULL) {
        return;
    }

    while (__atomic_load_n(&g_window_armed, __ATOMIC_SEQ_CST) == 0 && waited < 2000) {
        k_sched_sleep_ms(1);
        waited++;
    }

    KT_CHECK_INT_EQ(__atomic_load_n(&g_window_armed, __ATOMIC_SEQ_CST), 1);

    /* The waiter is in its turn, the record armed and the sleep not begun. */
    k_sched_sleep_ms(5);

    start = k_port_uptime_us();
    k_wait_wake(K_WAIT_CHANNEL_CONSOLE);

    while (__atomic_load_n(&g_window_result, __ATOMIC_SEQ_CST) == 0x7FFFFFF && waited < 4000) {
        k_sched_sleep_ms(1);
        waited++;
    }

    spent_ms = (k_port_uptime_us() - start) / 1000u;

    KT_CHECK_INT_EQ(__atomic_load_n(&g_window_result, __ATOMIC_SEQ_CST), K_WAIT_WOKEN);
    /* The wake has to be seen long before the two seconds of the timeout. */
    KT_CHECK(spent_ms < 1000u);

    (void)task_wait(waiter->pid, NULL, 2000);
}

/* --- the latency of a wake --- */

static atomic_int g_armed;
static atomic_int g_awake;
static atomic_int g_wait_answer;
static atomic_uint_fast64_t g_awake_stamp;

static void latency_waiter(void *arg)
{
    K_UNUSED(arg);

    (void)k_wait_arm(&g_test_channel, NULL);
    (void)__atomic_store_n(&g_armed, 1, __ATOMIC_SEQ_CST);

    (void)__atomic_store_n(&g_wait_answer, k_wait_sleep(2000), __ATOMIC_SEQ_CST);
    (void)__atomic_store_n(&g_awake_stamp, k_port_uptime_us(), __ATOMIC_SEQ_CST);
    (void)__atomic_store_n(&g_awake, 1, __ATOMIC_SEQ_CST);

    k_wait_disarm();
}

static void test_wake_latency(void)
{
    struct task *waiter;
    k_u64_t stamp;
    k_u64_t spent_us;
    unsigned i;
    unsigned samples = 5;
    unsigned long long best = ~0ull;
    int waited = 0;

    ktest_begin("wait: a wake of a sleeping task is immediate");

    for (i = 0; i < samples; i++) {
        (void)__atomic_store_n(&g_armed, 0, __ATOMIC_SEQ_CST);
        (void)__atomic_store_n(&g_awake, 0, __ATOMIC_SEQ_CST);
        (void)__atomic_store_n(&g_wait_answer, 0x7FFFFFF, __ATOMIC_SEQ_CST);

        waiter = task_create("wt-lat", latency_waiter, NULL, TASK_PRIO_DEFAULT + 1);
        KT_CHECK(waiter != NULL);

        if (waiter == NULL) {
            return;
        }

        while (__atomic_load_n(&g_armed, __ATOMIC_SEQ_CST) == 0 && waited < 2000) {
            k_sched_sleep_ms(1);
            waited++;
        }

        /* Long enough for the waiter to be asleep in the port, not only
         * armed: the number is the wake of a sleeping task. */
        k_sched_sleep_ms(20);

        stamp = k_port_uptime_us();
        k_wait_wake(&g_test_channel);

        while (__atomic_load_n(&g_awake, __ATOMIC_SEQ_CST) == 0 && waited < 4000) {
            k_sched_sleep_ms(1);
            waited++;
        }

        spent_us = (k_u64_t)__atomic_load_n(&g_awake_stamp, __ATOMIC_SEQ_CST) - stamp;

        KT_CHECK_INT_EQ(__atomic_load_n(&g_wait_answer, __ATOMIC_SEQ_CST), K_WAIT_WOKEN);
        KT_CHECK(spent_us < TEST_WAKE_LIMIT_US);

        if (spent_us < best) {
            best = spent_us;
        }

        (void)task_wait(waiter->pid, NULL, 2000);
    }

    printf("wait: the best wake of a sleeping task is %llu us on the %s port\n", best,
           k_port_name());
}

/* --- the round trip of a pipe --- */

/*
 * The two ends of the pipe of the test and the samples the reader filled in.
 * The shape is the one of `bench latpipe` (ports/port_x86_64/programs/
 * bench.c): the writer stamps the clock before the write, the reader stamps
 * the clock after the read and sends the stamp back, and the age of the byte
 * is the round trip with the wake of the reader in it.
 */
#define PIPE_SAMPLES 64
#define PIPE_STAMP_SIZE (2u * sizeof(k_u64_t))

static struct kfile *g_data_w;
static struct kfile *g_data_r;
static struct kfile *g_ack_w;
static struct kfile *g_ack_r;

static void pipe_reader(void *arg)
{
    unsigned i;

    K_UNUSED(arg);

    for (i = 0; i < PIPE_SAMPLES; i++) {
        k_u64_t stamp[2];

        if (kfile_read(g_data_r, stamp, PIPE_STAMP_SIZE) != (k_ssize_t)PIPE_STAMP_SIZE) {
            return;
        }

        stamp[1] = k_port_uptime_us();

        if (kfile_write(g_ack_w, stamp, PIPE_STAMP_SIZE) != (k_ssize_t)PIPE_STAMP_SIZE) {
            return;
        }
    }
}

static void test_wait_pipe_round_trip(void)
{
    struct task *reader;
    int data[2];
    int ack[2];
    unsigned i;
    unsigned long long samples[PIPE_SAMPLES];
    unsigned long long median;
    k_ssize_t rc;

    ktest_begin("wait: the round trip of a pipe of the host");

    KT_CHECK_INT_EQ(kfile_pipe(task_self(), data), 0);
    KT_CHECK_INT_EQ(kfile_pipe(task_self(), ack), 0);

    g_data_w = kfile_get(task_self(), data[1]);
    g_data_r = kfile_get(task_self(), data[0]);
    g_ack_w = kfile_get(task_self(), ack[1]);
    g_ack_r = kfile_get(task_self(), ack[0]);

    if (g_data_w == NULL || g_data_r == NULL || g_ack_w == NULL || g_ack_r == NULL) {
        KT_CHECK(false);
        return;
    }

    reader = task_create("wt-pipe", pipe_reader, NULL, TASK_PRIO_DEFAULT + 1);
    KT_CHECK(reader != NULL);

    if (reader == NULL) {
        return;
    }

    for (i = 0; i < PIPE_SAMPLES; i++) {
        k_u64_t sent[2] = {k_port_uptime_us(), 0};
        k_u64_t back[2];

        rc = kfile_write(g_data_w, sent, PIPE_STAMP_SIZE);
        KT_CHECK_INT_EQ(rc, (k_ssize_t)PIPE_STAMP_SIZE);

        rc = kfile_read(g_ack_r, back, PIPE_STAMP_SIZE);
        KT_CHECK_INT_EQ(rc, (k_ssize_t)PIPE_STAMP_SIZE);

        samples[i] = (back[1] > sent[0]) ? (back[1] - sent[0]) : 0ull;
    }

    (void)task_wait(reader->pid, NULL, 2000);

    /* The middle of the sorted samples, which is the `p50` of the bench. */
    for (i = 1; i < PIPE_SAMPLES; i++) {
        unsigned long long value = samples[i];
        unsigned j = i;

        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            j--;
        }

        samples[j] = value;
    }

    median = samples[PIPE_SAMPLES / 2];
    printf("wait: the p50 of a byte through a pipe is %llu us on the %s port\n", median,
           k_port_name());

    /* A tick was 1 006 us and the chunk of 1.8 ten thousand: the wait has to
     * be far below both on a port whose sleep is cut. */
    KT_CHECK(median < TEST_WAKE_LIMIT_US);

    (void)kfile_close(task_self(), data[0]);
    (void)kfile_close(task_self(), data[1]);
    (void)kfile_close(task_self(), ack[0]);
    (void)kfile_close(task_self(), ack[1]);
}

/* --- a sleep is never shorter than its argument --- */

/*
 * A nudge every two milliseconds, from another task, while the test sleeps: a
 * door of the port that lost its race with the sleep it was meant for (and a
 * port that rounds its timeout) must not make a sleep shorter than the time
 * the caller asked for. The pre-1.9 shape of the kernel slept once and
 * answered, so the first nudge ended a sleep of fifty milliseconds at once;
 * the sleep is a loop on the tick of its deadline now.
 */
static atomic_int g_nudge_running;

static void nudge_task(void *arg)
{
    struct task *sleeper = (struct task *)arg;

    while (__atomic_load_n(&g_nudge_running, __ATOMIC_SEQ_CST) != 0) {
        k_port_task_wake(sleeper);
        k_sched_sleep_ms(2);
    }
}

static void test_sleep_deadline(void)
{
    struct task *nudger;
    k_u64_t start;
    k_u64_t elapsed;

    ktest_begin("wait: a sleep is never shorter than its time");

    (void)__atomic_store_n(&g_nudge_running, 1, __ATOMIC_SEQ_CST);

    nudger = task_create("wt-nudge", nudge_task, task_self(), TASK_PRIO_DEFAULT + 1);
    KT_CHECK(nudger != NULL);

    if (nudger == NULL) {
        (void)__atomic_store_n(&g_nudge_running, 0, __ATOMIC_SEQ_CST);
        return;
    }

    start = k_port_uptime_us();
    k_sched_sleep_ms(50);
    elapsed = (k_port_uptime_us() - start) / 1000u;

    (void)__atomic_store_n(&g_nudge_running, 0, __ATOMIC_SEQ_CST);

    /* The nudger leaves at its next two milliseconds; the wait reaps it. */
    (void)task_wait(nudger->pid, NULL, 2000);

    KT_CHECK(elapsed >= 50u);

    printf("wait: the sleep of 50 ms under a nudge every 2 ms took %llu ms\n",
           (unsigned long long)elapsed);
}

void test_wait(void)
{
    test_wait_arm();
    test_wait_window();
    test_wake_latency();
    test_wait_pipe_round_trip();
    test_sleep_deadline();
}
