#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/config.h"
#include "kernel/log.h"
#include "port_host.h"

static void test_log_levels(void) {
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

    port_host_set_sink(NULL);
    fclose(sink);
}

static void test_log_ring(void) {
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

static void test_log_wrap(void) {
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

void test_log(void) {
    test_log_levels();
    test_log_ring();
    test_log_wrap();
}
