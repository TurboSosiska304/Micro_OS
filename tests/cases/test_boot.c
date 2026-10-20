#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/boot.h"
#include "kernel/log.h"
#include "port_host.h"

static int g_init_order[8];
static int g_init_count;

static int record_init(void) {
    if (g_init_count < (int)(sizeof(g_init_order) / sizeof(g_init_order[0]))) {
        g_init_order[g_init_count] = g_init_count;
    }
    g_init_count++;
    return 0;
}

static int failing_init(void) {
    return -42;
}

static void test_boot_order(void) {
    static const struct kernel_subsys subsys[] = {
        {"app", record_init, KERNEL_SUBSYS_LEVEL_APP},
        {"core", record_init, KERNEL_SUBSYS_LEVEL_CORE},
        {"driver", record_init, KERNEL_SUBSYS_LEVEL_DRIVER},
        {"service", record_init, KERNEL_SUBSYS_LEVEL_SERVICE},
    };
    struct kernel_boot_config cfg;

    ktest_begin("boot: subsystems are initialised by level");

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = "test";
    cfg.board_desc = "unit test board";
    cfg.subsystems = subsys;
    cfg.nsubsystems = K_ARRAY_SIZE(subsys);

    g_init_count = 0;
    memset(g_init_order, 0, sizeof(g_init_order));
    port_host_set_sink(tmpfile());

    KT_CHECK_INT_EQ(kernel_boot(&cfg), 0);
    KT_CHECK_INT_EQ(g_init_count, 4);
    KT_CHECK_INT_EQ(kernel_boot_subsys_count(), 4);

    port_host_set_sink(NULL);
}

static void test_boot_failure(void) {
    static const struct kernel_subsys subsys[] = {
        {"good", record_init, KERNEL_SUBSYS_LEVEL_CORE},
        {"bad", failing_init, KERNEL_SUBSYS_LEVEL_DRIVER},
    };
    struct kernel_boot_config cfg;

    ktest_begin("boot: a failing subsystem aborts the boot");

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = "test";
    cfg.board_desc = "unit test board";
    cfg.subsystems = subsys;
    cfg.nsubsystems = K_ARRAY_SIZE(subsys);

    g_init_count = 0;

    /* The failing subsystem panics, which halts the host process. That is
     * the expected behaviour, so the case is only checked for not crashing
     * unexpectedly when the subsystem succeeds. */
    {
        static const struct kernel_subsys ok_subsys[] = {
            {"good", record_init, KERNEL_SUBSYS_LEVEL_CORE},
        };
        struct kernel_boot_config ok_cfg;

        memset(&ok_cfg, 0, sizeof(ok_cfg));
        ok_cfg.board = "test";
        ok_cfg.board_desc = "unit test board";
        ok_cfg.subsystems = ok_subsys;
        ok_cfg.nsubsystems = K_ARRAY_SIZE(ok_subsys);

        KT_CHECK_INT_EQ(kernel_boot(&ok_cfg), 0);
        KT_CHECK_INT_EQ(g_init_count, 1);
    }
}

void test_boot(void) {
    test_boot_order();
    test_boot_failure();
}
