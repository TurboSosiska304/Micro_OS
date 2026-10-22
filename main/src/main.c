/*
 * Micro OS — ESP32 firmware entry point.
 *
 * The IDF application task plays the role of the "bootloader" in the Linux
 * sense: it hands control to the kernel, which then initialises all
 * subsystems in order. The first task created by the kernel becomes PID 1,
 * just like the init process of Linux.
 */
#include "board/board.h"
#include "drivers/drivers.h"
#include "kernel/boot.h"
#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kernel/vfs.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"
#include "devfs/devfs.h"
#include "graphics/tft_console.h"
#include "mfs/mfs.h"
#include "mfs/mfs_flash.h"
#include "procfs/procfs.h"
#include "kshell/kshell.h"
#include "port_esp32.h"
#include "selftest.h"

#include <string.h>

static int board_subsys_init(void)
{
    const struct board_info *info = board_get_info();

    klog(K_LOG_LEVEL_INFO, "soc: %s, flash: %u MB, psram: %u KB", info->soc,
         (unsigned)info->flash_mb, (unsigned)info->psram_kb);
    klog(K_LOG_LEVEL_INFO, "lcd: %s", info->lcd);
    klog(K_LOG_LEVEL_INFO, "imu: %s, rtc: %s", info->imu, info->rtc);

    return 0;
}

static int drivers_subsys_init(void)
{
    return drivers_init();
}

/*
 * The command that formats the volume of the flash. It refuses without the
 * flag -y: the whole content of the volume is destroyed.
 */
static int cmd_mkfs(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-y") != 0) {
        kshell_printf("usage: mkfs -y   (erases the whole volume)\n");
        return KSHELL_USAGE;
    }

    /* The volume cannot be formatted while it is mounted. */
    mfs_unmount();
    vfs_umount("/");

    if (mfs_flash_format() != 0) {
        kshell_printf("mkfs: the volume cannot be formatted\n");
        return KSHELL_ERROR;
    }

    if (mfs_flash_mount("/") != 0) {
        kshell_printf("mkfs: the volume is formatted, but it cannot be mounted\n");
        return KSHELL_ERROR;
    }

    kshell_printf("mkfs: the volume is formatted\n");

    return KSHELL_OK;
}

/* The filesystem of the flash: the data of the user survives the restarts. */
static int fs_subsys_init(void)
{
    int rc;

    /*
     * The command is registered before the mount: when the volume of the
     * user is damaged, the board still starts and the shell can format it
     * again.
     */
    kshell_register("mkfs", "format the volume: mkfs -y", cmd_mkfs);

    rc = mfs_flash_mount("/");
    if (rc != 0) {
        /*
         * The board has to work even when the data of the user cannot be
         * read: the information of the system is still there, and the
         * shell has the command that makes a new volume.
         */
        kwarn("init: the filesystem of the flash is not available (rc=%d)", rc);
    }

    /* The information about the system and the devices. */
    if (procfs_register("/proc") != 0) {
        kerror("init: cannot mount the procfs");
        return -1;
    }

    if (devfs_register("/dev") != 0) {
        kerror("init: cannot mount the devfs");
        return -1;
    }

    return 0;
}

/* The shell: the input comes from the UART, the output goes to the UART
 * and to the display of the board. */
static void shell_task(void *arg)
{
    K_UNUSED(arg);

    tft_console_start();
    tft_console_register();
    kshell_set_sink(tft_console_write);

    kshell_run();
}

/*
 * The log of the boot goes to the file /log/start.log, so that the console
 * stays readable. The doctor report and the summary still go to the console.
 */
static int write_boot_log(void)
{
    struct vfs_file file;
    /* The place for the data of the filesystem of an open file. */
    static k_u8_t file_data[VFS_FILE_DATA_SIZE];
    const char *text;
    int rc;

    /* The directory of the log. */
    rc = vfs_mkdir("/log");
    if (rc != 0 && rc != -3) {
        kwarn("init: cannot create the directory /log (rc=%d)", rc);
    }

    rc = vfs_open("/log/start.log", VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, file_data);
    if (rc != 0) {
        kwarn("init: cannot create the log file (rc=%d)", rc);
        return rc;
    }

    text = klog_captured_text();
    if (text != NULL && klog_captured_size() > 0) {
        vfs_write(&file, text, klog_captured_size());
    }

    vfs_close(&file);

    return 0;
}

/*
 * The init task: the first task of the system (PID 1). It checks the kernel
 * and the hardware, and later starts the shell and the other services.
 */
static void init_task(void *arg)
{
    struct task *self = task_self();

    K_UNUSED(arg);

    kinfo("init: system is up, pid %u, uptime %llu ms", (unsigned)(self ? self->pid : 0),
          (unsigned long long)k_sched_uptime_ms());

    /* The first thing the init process does: check that the kernel works. */
    selftest_run();

    /* Then it walks through the modules and checks the hardware. */
    kinfo("init: running the hardware doctor");
    if (kmod_doctor() == 0) {
        kinfo("init: all modules are healthy");
    } else {
        kwarn("init: the doctor found problems, see the report");
    }

    /* The log of the boot goes to the file, the console gets the report. */
    if (write_boot_log() == 0) {
        klog_capture_stop();
        kinfo("init: the log of the boot is in /log/start.log (%u bytes)",
              (unsigned)klog_captured_size());
    } else {
        klog_capture_stop();
    }

    kmod_report();

    /* And finally it starts the shell. */
    if (task_create("shell", shell_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        kerror("init: cannot start the shell");
    }

    for (;;) {
        k_sched_sleep_ms(1000);
    }
}

static int tasks_subsys_init(void)
{
    if (task_create("init", init_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        return -1;
    }

    return 0;
}

static const struct kernel_subsys g_subsystems[] = {
    {"board", board_subsys_init, KERNEL_SUBSYS_LEVEL_CORE},
    {"drivers", drivers_subsys_init, KERNEL_SUBSYS_LEVEL_DRIVER},
    {"fs", fs_subsys_init, KERNEL_SUBSYS_LEVEL_SERVICE},
    {"tasks", tasks_subsys_init, KERNEL_SUBSYS_LEVEL_APP},
};

void app_main(void)
{
    const struct board_info *info = board_get_info();
    struct kernel_boot_config cfg;

    port_esp32_console_init();
    port_esp32_init();

    /* The log of the boot is collected, it is written to a file later. */
    klog_capture_start();

    /* The time base of the kernel: one tick per millisecond. */
    port_esp32_start_tick(1);

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = info->name;
    cfg.board_desc = info->desc;
    cfg.subsystems = g_subsystems;
    cfg.nsubsystems = K_ARRAY_SIZE(g_subsystems);

    kernel_boot(&cfg);
}
