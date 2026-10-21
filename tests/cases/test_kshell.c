#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

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

void test_kshell(void)
{
    /* The built in commands are registered by the shell itself. */
    KT_CHECK_INT_EQ(kshell_init(), KSHELL_OK);

    test_execute();
    test_unknown();
    test_registry();
    test_memtest();
}
