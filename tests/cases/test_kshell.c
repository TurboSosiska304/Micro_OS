#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/version.h"
#include "kshell/kshell.h"
#include "port_host.h"

static int g_argc;
static char g_args[KSHELL_MAX_ARGS][32];
static int g_result;

static int test_command(int argc, char **argv)
{
    int i;

    g_argc = argc;
    for (i = 0; i < argc && i < KSHELL_MAX_ARGS; i++) {
        strncpy(g_args[i], argv[i], sizeof(g_args[i]) - 1);
        g_args[i][sizeof(g_args[i]) - 1] = '\0';
    }

    return g_result;
}

static void test_execute(void)
{
    ktest_begin("kshell: a command is executed with its arguments");

    port_host_set_sink(tmpfile());
    g_argc = 0;
    g_result = 42;

    KT_CHECK_INT_EQ(kshell_register("testcmd", "a command for the unit tests", test_command), 0);
    KT_CHECK_INT_EQ(kshell_execute("testcmd one two"), 42);
    KT_CHECK_INT_EQ(g_argc, 3);
    KT_CHECK_STR_EQ(g_args[0], "testcmd");
    KT_CHECK_STR_EQ(g_args[1], "one");
    KT_CHECK_STR_EQ(g_args[2], "two");

    /* Extra spaces must not produce empty arguments. */
    KT_CHECK_INT_EQ(kshell_execute("  testcmd   spaced  "), 42);
    KT_CHECK_INT_EQ(g_argc, 2);
    KT_CHECK_STR_EQ(g_args[1], "spaced");

    /* An empty line does nothing. */
    KT_CHECK_INT_EQ(kshell_execute("   "), KSHELL_OK);

    port_host_set_sink(NULL);
}

static void test_unknown(void)
{
    ktest_begin("kshell: an unknown command is reported");

    port_host_set_sink(tmpfile());

    KT_CHECK(kshell_execute("nosuchcommand") != KSHELL_OK);

    port_host_set_sink(NULL);
}

static void test_registry(void)
{
    ktest_begin("kshell: the commands can be registered and removed");

    port_host_set_sink(tmpfile());

    KT_CHECK(kshell_command_count() > 0);
    KT_CHECK_INT_EQ(kshell_register("testcmd", "a command for the unit tests", test_command), 0);
    KT_CHECK_INT_EQ(kshell_unregister("testcmd"), KSHELL_OK);
    KT_CHECK(kshell_execute("testcmd") != KSHELL_OK);

    /*
     * The table of the commands has a limited number of places, and a
     * command that does not fit is lost: the board lost "mount" and "df"
     * that way, without a word. The one that registers the commands says
     * when one of them was refused, and the test says how many commands
     * the board really has.
     */
    KT_CHECK_INT_EQ(kshell_register_files(), 0);

    /*
     * The board has more commands than the tests can see here: the
     * drivers bring their own ("date", "i2c", "wifi", "display", "mkfs")
     * and those are only on the board. The twenty three commands of the
     * shell itself have to be here, all of them.
     */
    KT_CHECK(kshell_command_count() >= 23);

    port_host_set_sink(NULL);
}

static void test_memtest(void)
{
    ktest_begin("kshell: the memory test measures the memory");

    port_host_set_sink(tmpfile());

    /* The command has to run on every platform, even without a PSRAM. */
    KT_CHECK_INT_EQ(kshell_execute("memtest"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("memtest 32"), KSHELL_OK);

    /* A size of zero is not a valid argument. */
    KT_CHECK(kshell_execute("memtest 0") != KSHELL_OK);

    port_host_set_sink(NULL);
}

/*
 * The report of `mfetch` is written to the sink of the port: the test
 * reads it back from there. The sink is closed by the port when it is
 * replaced, so the reading is done while it is still the current one.
 */
static void test_mfetch(void)
{
    char buf[4096];
    size_t used;
    FILE *sink;

    ktest_begin("kshell: mfetch prints the logo and the report");

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        return;
    }

    port_host_set_sink(sink);

    KT_CHECK_INT_EQ(kshell_execute("mfetch"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("mfetch color"), KSHELL_OK);

    /* The report has a shape of its own: an unknown argument is refused. */
    KT_CHECK(kshell_execute("mfetch nosuchmode") != KSHELL_OK);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    port_host_set_sink(NULL);

    /* The system and the board are named in the plain report ... */
    KT_CHECK(strstr(buf, kernel_name()) != NULL);
    KT_CHECK(strstr(buf, "Board") != NULL);
    KT_CHECK(strstr(buf, "command(s)") != NULL);

    /* ... the rows a person looks for ... */
    KT_CHECK(strstr(buf, "Build") != NULL);
    KT_CHECK(strstr(buf, "Volume") != NULL);
    KT_CHECK(strstr(buf, "Modules") != NULL);
    KT_CHECK(strstr(buf, "Heap") != NULL);

    /* ... and the colored logo brings the escapes of the terminal. */
    KT_CHECK(strstr(buf, "MM MM") != NULL);
    KT_CHECK(strstr(buf, "\033[") != NULL);
}

void test_kshell(void)
{
    /* The built in commands are registered by the shell itself. */
    KT_CHECK_INT_EQ(kshell_init(), KSHELL_OK);

    test_execute();
    test_unknown();
    test_registry();
    test_memtest();
    test_mfetch();
}
