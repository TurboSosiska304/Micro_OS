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
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"
#include "port_host.h"

static void heartbeat_task(void *arg) {
    K_UNUSED(arg);

    kinfo("heartbeat: started");

    for (;;) {
        kinfo("heartbeat: uptime %llu ms, %u task(s)", (unsigned long long)k_sched_uptime_ms(),
              (unsigned)task_count());
        k_sched_sleep_ms(2000);
    }
}

static void worker_task(void *arg) {
    int id = (int)(intptr_t)arg;
    int i;

    kinfo("worker %d: started", id);

    for (i = 0; i < 5; i++) {
        kinfo("worker %d: step %d", id, i);
        k_sched_sleep_ms(200);
    }

    kinfo("worker %d: done", id);
}

static int tasks_subsys_init(void) {
    if (task_create("heartbeat", heartbeat_task, NULL, 8) == NULL) {
        return -1;
    }

    if (task_create("worker0", worker_task, (void *)(intptr_t)0, 5) == NULL) {
        return -1;
    }

    if (task_create("worker1", worker_task, (void *)(intptr_t)1, 5) == NULL) {
        return -1;
    }

    return 0;
}

static const struct kernel_subsys g_subsystems[] = {
    {"tasks", tasks_subsys_init, KERNEL_SUBSYS_LEVEL_APP},
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

    /* The time base of the kernel: one tick per millisecond. */
    port_host_start_tick(1);

    kernel_boot(&cfg);

    repl();
    return 0;
}
