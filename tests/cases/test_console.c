/*
 * Micro OS — the console of the system.
 *
 * The console has one port and several sinks: the display of the board
 * is the first sink, and a client of telnet asks for its copy beside
 * it. The tests walk the list the way the boot does — the display
 * first, the others after — and check that the port still gets the
 * text and that a full list refuses instead of writing over somebody.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/console.h"
#include "port_host.h"

static int s_a_calls;
static int s_b_calls;
static int s_c_calls;
static char s_a_last[64];

static void sink_a(const char *data, k_size_t len)
{
    s_a_calls++;

    if (len > sizeof(s_a_last) - 1) {
        len = sizeof(s_a_last) - 1;
    }

    memcpy(s_a_last, data, len);
    s_a_last[len] = '\0';
}

static void sink_b(const char *data, k_size_t len)
{
    (void)data;
    (void)len;
    s_b_calls++;
}

static void sink_c(const char *data, k_size_t len)
{
    (void)data;
    (void)len;
    s_c_calls++;
}

void test_console_sinks(void)
{
    FILE *port_sink;
    char from_port[64];
    size_t n;

    ktest_begin("console: the text reaches the port and every sink");

    s_a_calls = 0;
    s_b_calls = 0;
    s_c_calls = 0;
    s_a_last[0] = '\0';

    port_sink = tmpfile();
    KT_CHECK(port_sink != NULL);
    port_host_set_sink(port_sink);

    k_console_set_sink(sink_a);
    KT_CHECK_INT_EQ(k_console_add_sink(sink_b), 0);

    k_console_write("hello", 5);
    KT_CHECK_INT_EQ(s_a_calls, 1);
    KT_CHECK_INT_EQ(s_b_calls, 1);
    KT_CHECK_STR_EQ(s_a_last, "hello");

    /* The port got the same bytes. */
    rewind(port_sink);
    n = fread(from_port, 1, sizeof(from_port) - 1, port_sink);
    from_port[n] = '\0';
    KT_CHECK_STR_EQ(from_port, "hello");

    ktest_begin("console: a full list refuses instead of writing over somebody");

    /* The third slot is the last: the display and two more. */
    KT_CHECK_INT_EQ(k_console_add_sink(sink_c), 0);
    KT_CHECK_INT_EQ(k_console_add_sink(sink_a), -1);
    KT_CHECK_INT_EQ(k_console_add_sink(NULL), -1);

    s_a_calls = 0;
    s_b_calls = 0;
    s_c_calls = 0;
    k_console_write("again", 5);
    KT_CHECK_INT_EQ(s_a_calls, 1);
    KT_CHECK_INT_EQ(s_b_calls, 1);
    KT_CHECK_INT_EQ(s_c_calls, 1);

    ktest_begin("console: nothing of an empty or absent write is sent");

    s_a_calls = 0;
    s_b_calls = 0;
    s_c_calls = 0;
    k_console_write(NULL, 4);
    k_console_write("x", 0);
    KT_CHECK_INT_EQ(s_a_calls, 0);
    KT_CHECK_INT_EQ(s_b_calls, 0);
    KT_CHECK_INT_EQ(s_c_calls, 0);

    /* The display keeps its slot even when the others came first. */
    k_console_set_sink(sink_a);
    k_console_write("mine", 4);
    KT_CHECK_STR_EQ(s_a_last, "mine");

    k_console_set_sink(NULL);
    port_host_set_sink(NULL);
}
