/*
 * Micro OS — the entry point on a QEMU x86-64 machine.
 *
 * Limine hands the kernel over in long mode with a stack and a map of the
 * memory; from here on the boot is the same as on the board: the kernel
 * checks the contract of the port, prints its banner and initialises the
 * subsystems it was given.
 *
 * The first milestone has no subsystems and no tasks: it proves the whole
 * chain from the bootloader to the log of the kernel, and then stops. The
 * task substrate is the next one.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/board.h"
#include "kernel/boot.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/power.h"

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
    kinfo("x86-64: the first milestone is done; the task substrate comes next");

    k_halt();
}
