/*
 * Micro OS — the job table of the shell.
 *
 * The console smoke drives Ctrl+Z and Ctrl+C through QEMU. The host test
 * checks the same table without a keyboard: a background pipeline is not
 * waited for, SIGSTOP changes the displayed state, bg continues it, fg takes
 * the terminal and waits, and every finished stage is reaped.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/sched.h"
#include "kernel/task.h"
#include "kshell/kshell.h"
#include "port_host.h"

static FILE *sink_open(void)
{
    FILE *sink = tmpfile();

    KT_CHECK(sink != NULL);
    if (sink != NULL) {
        port_host_set_sink(sink);
    }

    return sink;
}

static void sink_close(FILE *sink, char *buf, k_size_t size)
{
    size_t used;

    rewind(sink);
    used = fread(buf, 1, size - 1, sink);
    buf[used] = '\0';

    /* The port owns the stream and closes it when the sink is removed. */
    port_host_set_sink(NULL);
}

static int run_capture(const char *line, char *buf, k_size_t size)
{
    FILE *sink = sink_open();
    int rc;

    if (sink == NULL) {
        buf[0] = '\0';
        return KSHELL_ERROR;
    }

    rc = kshell_execute(line);
    sink_close(sink, buf, size);
    return rc;
}

/* The launch line is `[job] pid`; the table line that follows carries the
 * same number and pid. */
static bool launched_job(const char *text, unsigned *number, unsigned *pid)
{
    const char *at = strchr(text, '[');
    char *end = NULL;
    unsigned long value;

    if (at == NULL) {
        return false;
    }

    value = strtoul(at + 1, &end, 10);
    if (end == at + 1 || *end != ']' || value == 0 || value > 0xFFFFFFFFu) {
        return false;
    }
    *number = (unsigned)value;

    at = end + 1;
    while (*at == ' ') {
        at++;
    }

    value = strtoul(at, &end, 10);
    if (end == at || value == 0 || value > 0xFFFFFFFFu) {
        return false;
    }
    *pid = (unsigned)value;

    return true;
}

static void wait_done(unsigned number, char *buf, k_size_t size)
{
    char marker[32];
    int waited;

    snprintf(marker, sizeof(marker), "[%u]", number);

    for (waited = 0; waited < 200; waited++) {
        const char *row;

        (void)run_capture("jobs", buf, size);
        row = strstr(buf, marker);
        if (row != NULL && strstr(row, "Done") != NULL) {
            return;
        }
        k_sched_sleep_ms(5);
    }

    KT_CHECK(strstr(buf, "Done") != NULL);
}

static void test_background_stop_bg(void)
{
    char buf[4096];
    char command[64];
    k_size_t tasks_before = task_count();
    k_u64_t started;
    unsigned number = 0;
    unsigned pid = 0;
    int rc;

    ktest_begin("kshell: a background job is listed, stopped and continued");

    started = k_sched_uptime_ms();
    rc = run_capture("sleep 1000 &", buf, sizeof(buf));
    KT_CHECK_INT_EQ(rc, KSHELL_OK);
    KT_CHECK(k_sched_uptime_ms() - started < 500u);
    KT_CHECK(launched_job(buf, &number, &pid));

    rc = run_capture("jobs", buf, sizeof(buf));
    KT_CHECK_INT_EQ(rc, KSHELL_OK);
    KT_CHECK(strstr(buf, "Running") != NULL);
    KT_CHECK(strstr(buf, "sleep 1000") != NULL);

    snprintf(command, sizeof(command), "kill -STOP %u", pid);
    KT_CHECK_INT_EQ(run_capture(command, buf, sizeof(buf)), KSHELL_OK);

    (void)run_capture("jobs", buf, sizeof(buf));
    KT_CHECK(strstr(buf, "Stopped") != NULL);

    snprintf(command, sizeof(command), "bg %%%u", number);
    rc = run_capture(command, buf, sizeof(buf));
    KT_CHECK_INT_EQ(rc, KSHELL_OK);
    KT_CHECK(strstr(buf, "Running") != NULL);

    (void)run_capture("jobs", buf, sizeof(buf));
    KT_CHECK(strstr(buf, "Running") != NULL);

    /* Stop it once more so TERM can be queued without leaving a plain sleep
     * of the builtin to run to its deadline. bg then resumes the task, the
     * pending TERM cuts the wait, and the stage exits. */
    snprintf(command, sizeof(command), "kill -STOP %u", pid);
    KT_CHECK_INT_EQ(run_capture(command, buf, sizeof(buf)), KSHELL_OK);
    snprintf(command, sizeof(command), "kill -TERM %u", pid);
    KT_CHECK_INT_EQ(run_capture(command, buf, sizeof(buf)), KSHELL_OK);
    snprintf(command, sizeof(command), "bg %%%u", number);
    KT_CHECK_INT_EQ(run_capture(command, buf, sizeof(buf)), KSHELL_OK);

    wait_done(number, buf, sizeof(buf));
    KT_CHECK(task_count() <= tasks_before);
}

static void test_foreground_wait_and_usage(void)
{
    char buf[4096];
    char command[64];
    unsigned number = 0;
    unsigned pid = 0;
    int rc;

    ktest_begin("kshell: fg takes the terminal, waits and returns the job status");

    rc = run_capture("sleep 100 &", buf, sizeof(buf));
    KT_CHECK_INT_EQ(rc, KSHELL_OK);
    KT_CHECK(launched_job(buf, &number, &pid));

    snprintf(command, sizeof(command), "fg %%%u", number);
    rc = run_capture(command, buf, sizeof(buf));
    KT_CHECK_INT_EQ(rc, KSHELL_OK);
    KT_CHECK(strstr(buf, "Continued") != NULL);
    KT_CHECK(strstr(buf, "sleep 100") != NULL);

    (void)run_capture("jobs", buf, sizeof(buf));
    KT_CHECK(strstr(buf, "Done") != NULL);

    KT_CHECK_INT_EQ(run_capture("fg nope", buf, sizeof(buf)), KSHELL_USAGE);
    KT_CHECK_INT_EQ(run_capture("bg %0", buf, sizeof(buf)), KSHELL_USAGE);
    KT_CHECK_INT_EQ(run_capture("jobs extra", buf, sizeof(buf)), KSHELL_USAGE);
}

void test_kshell_jobs(void)
{
    test_background_stop_bg();
    test_foreground_wait_and_usage();
}
