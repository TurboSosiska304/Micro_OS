#include "ktest.h"
#include "tests/tests.h"

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "kernel/boot.h"
#include "kernel/log.h"
#include "port_host.h"

#define MAX_RECORDED 8

static const char *g_order[MAX_RECORDED];
static int g_order_count;

static void record(const char *name)
{
    if (g_order_count < MAX_RECORDED) {
        g_order[g_order_count++] = name;
    }
}

static int init_app(void)
{
    record("app");
    return 0;
}

static int init_core(void)
{
    record("core");
    return 0;
}

static int init_driver(void)
{
    record("driver");
    return 0;
}

static int init_service(void)
{
    record("service");
    return 0;
}

static void test_boot_order(void)
{
    static const struct kernel_subsys subsys[] = {
        {"app", init_app, KERNEL_SUBSYS_LEVEL_APP},
        {"core", init_core, KERNEL_SUBSYS_LEVEL_CORE},
        {"driver", init_driver, KERNEL_SUBSYS_LEVEL_DRIVER},
        {"service", init_service, KERNEL_SUBSYS_LEVEL_SERVICE},
    };
    struct kernel_boot_config cfg;

    ktest_begin("boot: subsystems are initialised by level");

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = "test";
    cfg.board_desc = "unit test board";
    cfg.subsystems = subsys;
    cfg.nsubsystems = K_ARRAY_SIZE(subsys);

    g_order_count = 0;
    memset(g_order, 0, sizeof(g_order));
    port_host_set_sink(tmpfile());

    KT_CHECK_INT_EQ(kernel_boot(&cfg), 0);
    /* Four subsystems of the platform plus the built-in "sched". */
    KT_CHECK_INT_EQ(kernel_boot_subsys_count(), 5);

    KT_CHECK_INT_EQ(g_order_count, 4);
    if (g_order_count == 4) {
        KT_CHECK_STR_EQ(g_order[0], "core");
        KT_CHECK_STR_EQ(g_order[1], "driver");
        KT_CHECK_STR_EQ(g_order[2], "service");
        KT_CHECK_STR_EQ(g_order[3], "app");
    }

    port_host_set_sink(NULL);
}

static int init_broken(void)
{
    return -1;
}

/*
 * The property: a subsystem that fails stops the boot, and the board does
 * not go on with a half made system. The panic of the kernel halts the
 * process of the host, so the failing boot is run in a child process and
 * the code it stopped with is the one the port says it stops with.
 */
static void test_boot_failure(void)
{
    static const struct kernel_subsys ok_subsys[] = {
        {"good", init_core, KERNEL_SUBSYS_LEVEL_CORE},
    };
    struct kernel_boot_config cfg;

    ktest_begin("boot: a failing subsystem aborts the boot");

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = "test";
    cfg.board_desc = "unit test board";
    cfg.subsystems = ok_subsys;
    cfg.nsubsystems = K_ARRAY_SIZE(ok_subsys);

    g_order_count = 0;
    port_host_set_sink(tmpfile());

    /* The successful path is checked in the process of the test itself. */
    KT_CHECK_INT_EQ(kernel_boot(&cfg), 0);
    KT_CHECK_INT_EQ(kernel_boot_subsys_count(), 2);

    port_host_set_sink(NULL);
}

static void test_boot_failure_aborts(void)
{
    static const struct kernel_subsys bad_subsys[] = {
        {"broken", init_broken, KERNEL_SUBSYS_LEVEL_CORE},
    };
    struct kernel_boot_config cfg;
    pid_t child;
    int status = 0;

    ktest_begin("boot: a subsystem that fails stops the board");

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = "test";
    cfg.board_desc = "unit test board";
    cfg.subsystems = bad_subsys;
    cfg.nsubsystems = K_ARRAY_SIZE(bad_subsys);

    /*
     * The tick thread is stopped for the moment of the fork: a thread that
     * is not there in the child would leave a lock of the kernel taken
     * forever.
     */
    port_host_stop_tick();

    child = fork();
    KT_CHECK(child >= 0);

    if (child == 0) {
        /* The child: the panic stops it, the message goes nowhere. */
        (void)freopen("/dev/null", "w", stdout);
        (void)freopen("/dev/null", "w", stderr);

        (void)kernel_boot(&cfg);

        /* The boot must not come back from a failing subsystem. */
        _exit(0);
    }

    if (child > 0) {
        const int expected = 89; /* the code the port of the host halts with */

        KT_CHECK_INT_EQ(waitpid(child, &status, 0), child);
        KT_CHECK(WIFEXITED(status));
        KT_CHECK_INT_EQ(WEXITSTATUS(status), expected);
    }

    port_host_start_tick(1);
}

void test_boot(void)
{
    test_boot_order();
    test_boot_failure();
    test_boot_failure_aborts();
}
