/*
 * Micro OS — host simulator entry point.
 *
 * Boots the very same kernel core that runs on the device, but on Linux.
 * This gives us a fast edit/compile/run loop while the ESP32 toolchain is
 * not installed, and a place to exercise the shell and the VFS.
 */
#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "kernel/boot.h"
#include "kernel/log.h"
#include "kernel/version.h"

static int sim_subsys_init(void) {
    klog(K_LOG_LEVEL_INFO, "simulator: keyboard input is not implemented yet");
    return 0;
}

static const struct kernel_subsys g_subsystems[] = {
    {"sim", sim_subsys_init, KERNEL_SUBSYS_LEVEL_CORE},
};

static void repl(void) {
    char line[128];

    printf("microos> ");
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (len == 0) {
            printf("microos> ");
            fflush(stdout);
            continue;
        }

        printf("microos: unknown command: '%s' (the shell arrives in phase 2)\n", line);
        printf("microos> ");
        fflush(stdout);
    }
}

int main(void) {
    const struct board_info *info = board_get_info();
    struct kernel_boot_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = info->name;
    cfg.board_desc = info->desc;
    cfg.subsystems = g_subsystems;
    cfg.nsubsystems = K_ARRAY_SIZE(g_subsystems);

    kernel_boot(&cfg);

    repl();
    return 0;
}
