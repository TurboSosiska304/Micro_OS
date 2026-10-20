/*
 * Micro OS — ESP32 firmware entry point.
 *
 * The IDF application task plays the role of the "bootloader" in the Linux
 * sense: it hands control to the kernel, which then initialises all
 * subsystems in order. The first task created by the kernel becomes PID 1,
 * just like the init process of Linux.
 */
#include "board/board.h"
#include "kernel/boot.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"
#include "port_esp32.h"
#include "selftest.h"

#include <string.h>

static int board_subsys_init(void) {
    const struct board_info *info = board_get_info();

    klog(K_LOG_LEVEL_INFO, "soc: %s, flash: %u MB, psram: %u KB", info->soc,
         (unsigned)info->flash_mb, (unsigned)info->psram_kb);
    klog(K_LOG_LEVEL_INFO, "lcd: %s", info->lcd);
    klog(K_LOG_LEVEL_INFO, "imu: %s, rtc: %s", info->imu, info->rtc);

    return 0;
}

/*
 * The init task: the first task of the system (PID 1). It keeps the system
 * alive and will later start the shell and the other services.
 */
static void init_task(void *arg) {
    struct task *self = task_self();

    K_UNUSED(arg);

    kinfo("init: system is up, pid %u, uptime %llu ms", (unsigned)(self ? self->pid : 0),
          (unsigned long long)k_sched_uptime_ms());

    /* The first thing the init process does: check that the kernel works. */
    selftest_run();

    for (;;) {
        k_sched_sleep_ms(1000);
    }
}

static int tasks_subsys_init(void) {
    if (task_create("init", init_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        return -1;
    }

    return 0;
}

static const struct kernel_subsys g_subsystems[] = {
    {"board", board_subsys_init, KERNEL_SUBSYS_LEVEL_CORE},
    {"tasks", tasks_subsys_init, KERNEL_SUBSYS_LEVEL_APP},
};

void app_main(void) {
    const struct board_info *info = board_get_info();
    struct kernel_boot_config cfg;

    port_esp32_console_init();
    port_esp32_init();

    /* The time base of the kernel: one tick per millisecond. */
    port_esp32_start_tick(1);

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = info->name;
    cfg.board_desc = info->desc;
    cfg.subsystems = g_subsystems;
    cfg.nsubsystems = K_ARRAY_SIZE(g_subsystems);

    kernel_boot(&cfg);
}
