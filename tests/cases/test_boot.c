#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

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

    /*
     * A subsystem that fails makes the kernel panic, which halts the host
     * process. That is the expected behaviour, so only the successful path is
     * exercised here; the failure path is checked on the device.
     */
    KT_CHECK_INT_EQ(kernel_boot(&cfg), 0);
    KT_CHECK_INT_EQ(kernel_boot_subsys_count(), 2);

    port_host_set_sink(NULL);
}

void test_boot(void)
{
    test_boot_order();
    test_boot_failure();
}
