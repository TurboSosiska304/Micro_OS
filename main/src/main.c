/*
 * Micro OS — ESP32 firmware entry point.
 *
 * The IDF application task plays the role of the "bootloader" in the Linux
 * sense: it hands control to the kernel, which then initialises all
 * subsystems in order.
 */
#include "board/board.h"
#include "kernel/boot.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/version.h"
#include "port_esp32.h"

#include <string.h>

static int board_subsys_init(void) {
    const struct board_info *info = board_get_info();

    klog(K_LOG_LEVEL_INFO, "soc: %s, flash: %u MB, psram: %u KB", info->soc,
         (unsigned)info->flash_mb, (unsigned)info->psram_kb);
    klog(K_LOG_LEVEL_INFO, "lcd: %s", info->lcd);
    klog(K_LOG_LEVEL_INFO, "imu: %s, rtc: %s", info->imu, info->rtc);

    return 0;
}

static const struct kernel_subsys g_subsystems[] = {
    {"board", board_subsys_init, KERNEL_SUBSYS_LEVEL_CORE},
};

void app_main(void) {
    const struct board_info *info = board_get_info();
    struct kernel_boot_config cfg;

    port_esp32_console_init();

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = info->name;
    cfg.board_desc = info->desc;
    cfg.subsystems = g_subsystems;
    cfg.nsubsystems = K_ARRAY_SIZE(g_subsystems);

    kernel_boot(&cfg);
}
