/*
 * Micro OS — the entry point on a QEMU x86-64 machine.
 *
 * The loader hands the kernel over in long mode with a stack and a map of
 * the memory; from here on the boot is the same as on the board: the kernel
 * checks the contract of the port, prints its banner, initialises the
 * subsystems it was given and leaves the machine to the init process.
 *
 * The init process starts the shell and then idles. Until the task substrate
 * of the port exists there is no init process at all, and the machine says
 * so and keeps breathing instead of stopping without a word: a kernel that
 * stays alive is the difference between "the tasks are not done yet" and
 * "the boot is broken".
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/board.h"
#include "kernel/boot.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"

#include "kshell/kshell.h"

/* The body of the shell task, in src/shell.c. */
void port_x86_64_shell_run(void);

/* The nap of the tasks of the port that have nothing to do right now. */
#define PORT_IDLE_MS 1000u

static const struct kernel_board g_board = {
    .name = "qemu_x86_64",
    .desc = "QEMU x86-64 machine",
    .soc = "QEMU virtio",
    .flash_mb = 0,
    .psram_kb = 0,
    .lcd = "none (the console is COM1)",
    .imu = "none",
    .rtc = "the clock of the port",
    .audio = "none",
    .power = "none",
    .buttons = "none",
    .has_buzzer = false,
    .has_ir = false,
    .has_mic = false,
};

static void shell_task(void *arg)
{
    K_UNUSED(arg);

    port_x86_64_shell_run();
}

/*
 * The init process of the port: PID 1. It registers the commands of the
 * shell before the shell task exists, so that the first line a person types
 * already finds them, and then stays alive as the parent of the shell the
 * way init does on Linux.
 */
static void init_task(void *arg)
{
    K_UNUSED(arg);

    if (kshell_init() != KSHELL_OK) {
        kwarn("x86-64: not every command of the shell is registered");
    }

    if (task_create("shell", shell_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        kerror("x86-64: cannot start the shell");
    }

    for (;;) {
        k_sched_sleep_ms(PORT_IDLE_MS);
    }
}

void kmain(k_u64_t multiboot_info)
{
    struct kernel_boot_config cfg;

    port_x86_64_init(multiboot_info);

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = g_board.name;
    cfg.board_desc = g_board.desc;
    cfg.board_info = &g_board;

    kernel_boot(&cfg);

    kinfo("x86-64: the kernel is up, heap %u KB free of %u KB",
          (unsigned)(k_port_heap_free() / 1024), (unsigned)(k_port_heap_total() / 1024));

    if (task_create("init", init_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        /*
         * The context switch and the timer are the next milestone of the
         * port. Saying it once and sleeping is honest, and it keeps the
         * console of the machine alive for whoever looks at it.
         */
        kinfo("x86-64: no tasks yet");

        for (;;) {
            k_sched_sleep_ms(PORT_IDLE_MS);
        }
    }

    /* The init process owns the rest of the boot: the shell comes from it. */
    for (;;) {
        k_sched_sleep_ms(PORT_IDLE_MS);
    }
}
