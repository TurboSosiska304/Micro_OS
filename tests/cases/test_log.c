#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/config.h"
#include "kernel/console.h"
#include "kernel/log.h"
#include "port_host.h"

static void test_log_levels(void)
{
    FILE *sink = tmpfile();

    ktest_begin("log: level filtering");
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);
    klog_init(K_LOG_LEVEL_WARN);

    klog(K_LOG_LEVEL_DEBUG, "debug message");
    klog(K_LOG_LEVEL_INFO, "info message");
    klog(K_LOG_LEVEL_WARN, "warn message");
    klog(K_LOG_LEVEL_ERROR, "error message");

    KT_CHECK_INT_EQ(klog_get_level(), K_LOG_LEVEL_WARN);
    KT_CHECK_INT_EQ(klog_line_count(), 2);

    /*
     * The sink belongs to the port from the moment it is given: it is
     * closed when it is replaced.
     */
    port_host_set_sink(NULL);
}

static void test_log_ring(void)
{
    char buf[512];
    k_size_t used;

    ktest_begin("log: ring buffer keeps the newest messages");

    klog_init(K_LOG_LEVEL_INFO);
    klog_clear();
    port_host_set_sink(tmpfile());

    klog(K_LOG_LEVEL_INFO, "first");
    klog(K_LOG_LEVEL_INFO, "second");
    klog(K_LOG_LEVEL_INFO, "third");

    used = klog_dump(buf, sizeof(buf));
    KT_CHECK(used > 0);
    KT_CHECK(strstr(buf, "first") != NULL);
    KT_CHECK(strstr(buf, "second") != NULL);
    KT_CHECK(strstr(buf, "third") != NULL);

    /* The dump must be ordered from the oldest to the newest message. */
    KT_CHECK(strstr(buf, "first") < strstr(buf, "second"));
    KT_CHECK(strstr(buf, "second") < strstr(buf, "third"));

    port_host_set_sink(NULL);
}

static void test_log_wrap(void)
{
    char buf[512];
    int i;

    ktest_begin("log: ring buffer overflow drops the oldest message");

    klog_init(K_LOG_LEVEL_INFO);
    klog_clear();
    port_host_set_sink(tmpfile());

    for (i = 0; i < (int)KERNEL_LOG_LINES + 4; i++) {
        klog(K_LOG_LEVEL_INFO, "line-%02d", i);
    }

    KT_CHECK_INT_EQ(klog_line_count(), KERNEL_LOG_LINES);

    klog_dump(buf, sizeof(buf));
    KT_CHECK(strstr(buf, "line-00") == NULL);
    KT_CHECK(strstr(buf, "line-03") == NULL);
    KT_CHECK(strstr(buf, "line-04") != NULL);
    KT_CHECK(strstr(buf, "line-35") != NULL);

    port_host_set_sink(NULL);
}

static void test_log_colors(void)
{
    char buf[512];
    size_t used;
    FILE *sink = tmpfile();

    ktest_begin("log: the level is a colour on a console that reads it");

    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    klog_init(K_LOG_LEVEL_INFO);
    klog_clear();
    port_host_set_sink(sink);

    /*
     * The port of the host is a plain serial line and does not turn the
     * colours on; a test of the log does, because the mark of a level is the
     * kernel's and the bytes are the same for a screen. The ring keeps the
     * text without a sequence either way.
     */
    k_console_set_ansi(true);
    kwarn("warm %d", 1);
    kerror("bad %d", 2);
    kok("good %d", 3);
    kinfo("plain %d", 4);
    klog(K_LOG_LEVEL_DEBUG, "quiet %d", 5);
    k_console_set_ansi(false);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';
    port_host_set_sink(NULL);

    /* Amber for a warning, red for an error, green for a success, and the
     * information is the line itself. A level below the filter is not there
     * at all. */
    KT_CHECK(strstr(buf, "\033[33mwarm 1\033[0m\n") != NULL);
    KT_CHECK(strstr(buf, "\033[31mbad 2\033[0m\n") != NULL);
    KT_CHECK(strstr(buf, "\033[32mgood 3\033[0m\n") != NULL);
    KT_CHECK(strstr(buf, "\033[32mplain 4") == NULL && strstr(buf, "\033[33mplain 4") == NULL &&
             strstr(buf, "plain 4\n") != NULL);
    KT_CHECK(strstr(buf, "quiet 5") == NULL);

    /* The ring, which is what /proc/kmsg gives a person, has no sequence. */
    used = klog_dump(buf, sizeof(buf));
    KT_CHECK(used > 0);
    KT_CHECK(strstr(buf, "\033") == NULL);
    KT_CHECK(strstr(buf, "warm 1") != NULL);
}

static void test_log_capture(void)
{
    FILE *sink = tmpfile();
    char buf[128];
    size_t used;

    ktest_begin("log: a mirrored capture keeps the line and still prints it");

    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    klog_init(K_LOG_LEVEL_INFO);
    port_host_set_sink(sink);
    klog_capture_start_mirroring();
    kinfo("captured and printed");
    klog_capture_stop();

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1u, sink);
    buf[used] = '\0';
    port_host_set_sink(NULL);

    KT_CHECK_STR_EQ(klog_captured_text(), "captured and printed\n");
    KT_CHECK_INT_EQ((int)klog_captured_size(), 21);
    KT_CHECK_STR_EQ(buf, "captured and printed\n");
}

void test_log(void)
{
    test_log_levels();
    test_log_ring();
    test_log_wrap();
    test_log_colors();
    test_log_capture();
}
