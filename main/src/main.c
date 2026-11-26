/*
 * Micro OS — ESP32 firmware entry point.
 *
 * The IDF application task plays the role of the "bootloader" in the Linux
 * sense: it hands control to the kernel, which then initialises all
 * subsystems in order. The first task created by the kernel becomes PID 1,
 * just like the init process of Linux.
 */
#include "board/board.h"
#include "config/config.h"
#include "config/session.h"
#include "drivers/drivers.h"
#include "drivers/power.h"
#include "drivers/rtc.h"
#include "factory.h"
#include "kernel/boot.h"
#include "kernel/errno.h"
#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kernel/logfile.h"
#include "kernel/vfs.h"
#include "kernel/port.h"
#include "kernel/port/power.h"
#include "kernel/ksys.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"
#include "devfs/devfs.h"
#include "fs_ext2/ext2.h"
#include "graphics/tft_console.h"
#include "init/initd.h"
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"
#if MICRO_OS_FS_FAT32
#include "fs_fat32/fat.h"
#endif
#include "net/net.h"
#include "procfs/procfs.h"
#include "kshell/kshell.h"
#include "port_esp32.h"
#include "selftest.h"
#include "settings/settings.h"
#include "telnet/telnet.h"

#include <string.h>

static int board_subsys_init(void)
{
    const struct kernel_board *info = board_get_info();

    klog(K_LOG_LEVEL_INFO, "soc: %s, flash: %u MB, psram: %u KB", info->soc,
         (unsigned)info->flash_mb, (unsigned)info->psram_kb);
    klog(K_LOG_LEVEL_INFO, "lcd: %s", info->lcd);
    klog(K_LOG_LEVEL_INFO, "imu: %s, rtc: %s", info->imu, info->rtc);

    return 0;
}

static int drivers_subsys_init(void)
{
    if (drivers_init() != 0) {
        return -1;
    }

    /*
     * The components that the build can leave out (ADR 0010): a build
     * without the shell over the air or without the network registers no
     * command of it, and the boot still reaches the shell.
     */
#if MICRO_OS_TELNET
    /* The shell over the air: beside the drivers, not among them. */
    if (telnet_register() != 0) {
        return -1;
    }
#endif

#if MICRO_OS_NET
    /* The network services: the HTTP client and the clock of NTP. */
    return net_register();
#else
    return 0;
#endif
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

    if (mfs_vfs_format("flash0") != 0) {
        kshell_printf("mkfs: the volume cannot be formatted\n");
        return KSHELL_ERROR;
    }

    if (mfs_vfs_mount("/", "flash0") != 0) {
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
    (void)factory_register();

#if MICRO_OS_FS_FAT32
    /* A second filesystem is a type of the VFS: it is registered whether a
     * card is there or not, and the mount of a partition names it. */
    if (fat_vfs_register_type() != 0) {
        kwarn("init: the FAT filesystem could not register its type");
    }
#endif

#if CONFIG_MICRO_OS_FS_EXT2
    if (ext2_vfs_register_type() != 0) {
        kwarn("init: the ext2 filesystem could not register its type");
    }
#endif

    /* The volume of the flash is a type as well, so that a line of
     * /etc/fstab or a command of the shell can name it like any other. */
    if (mfs_vfs_register_type() != 0) {
        kwarn("init: MicroFS could not register its type");
    }

    rc = mfs_vfs_mount("/", "flash0");
    if (rc != 0) {
        /*
         * The board has to work even when the data of the user cannot be
         * read: the information of the system is still there, and the
         * shell has the command that makes a new volume.
         */
        kwarn("init: the filesystem of the flash is not available (rc=%d)", rc);
    }

    /*
     * The programs of the board: the `progs` partition, read-only and
     * mounted at /bin (ADR 0022). It is not a part of the volume of the
     * user, so it is mounted whether MicroFS is there or not; a board whose
     * partition was not flashed still reaches the shell and says so.
     */
    if (port_esp32_progs_mount() != 0) {
        kwarn("init: the programs of /bin are not available");
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

/*
 * The console of the board, once the boot is proven: the panel becomes a sink
 * of the text, the slot of the update is kept (ADR 0022), and init starts the
 * session that reads the UART. This is the getty of the board: a `logout`
 * ends a session and the loop of init starts another one as root.
 */
static void console_bring_up(void)
{
    tft_console_start();
    kshell_set_sink(tft_console_write);

    /*
     * The boot is proven: the filesystem, the progs partition, the self test,
     * the doctor and the console of the shell are up, so the slot of the
     * update is kept (ADR 0022). Before this line a reset returns the board
     * to the previous image; a failure here is a line on the console and not
     * a stopped shell.
     */
    (void)port_esp32_ota_confirm();
}

/*
 * The init task: the first task of the system (PID 1). It checks the kernel
 * and the hardware, and later starts the shell and the other services.
 */
static void init_task(void *arg)
{
    struct task *self = task_self();
    int restored;
    int defaults;
    int saved;
    int mounted;

    K_UNUSED(arg);

    kinfo("init: system is up, pid %u, uptime %llu ms", (unsigned)(self ? self->pid : 0),
          (unsigned long long)k_sched_uptime_ms());

    /*
     * Why the board is awake: "timer" or "ext1" says whether the last sleep
     * did what it was asked for, and "power-on" is a cold boot. The alarm of
     * the clock is a separate line of the log: the chip's timer is set for
     * its moment, so a wake by the timer with the flag of the alarm set is
     * the alarm doing its work.
     */
    kinfo("power: woke from %s", drv_power_wakeup_reason());

    if (drv_rtc_alarm_was_fired()) {
        if (strcmp(drv_power_wakeup_reason(), "timer") == 0 && drv_rtc_alarm_was_armed()) {
            /* The alarm is armed and the flag is set: the timer of the chip
             * was set for the match, see drivers/power.h. */
            kinfo("rtc: the alarm woke the board");
        } else {
            kinfo("rtc: the alarm went off before this boot");
        }
    }

    /* The first thing the init process does: check that the kernel works. */
    selftest_run();

    /* Then it walks through the modules and checks the hardware. */
    kinfo("init: running the hardware doctor");
    if (kmod_doctor() == 0) {
        kinfo("init: all modules are healthy");
    } else {
        kwarn("init: the doctor found problems, see the report");
    }

    /* The board keeps /etc in NVS and has no volume it may append to. The
     * captured boot and the console therefore stay in the same bounded RAM
     * log as a read-only root, and the line on the console says so. */
    if (k_logfile_start(klog_captured_text(), klog_captured_size(), false) != 0) {
        kerror("init: the logs cannot be installed");
    }
    klog_capture_stop();

    kmod_report();

    /*
     * The configuration from /etc: the NVS keeps the copy that survives a
     * restart and a formatted volume, and the boot brings the files back.
     * The task of the kernel has the stack for this, the boot task of the
     * IDF is too small for the buffers of the walk.
     */
    if (config_register() != 0) {
        kwarn("init: the config command is not registered");
    }

    /*
     * The store is installed by config_register(), so the restore below has
     * one: without it /etc would come back empty and the settings of the
     * board would be lost without a word.
     */
    if (config_store_get() != NULL) {
        restored = config_restore(CONFIG_DIR);

        if (restored < 0) {
            kwarn("init: the configuration cannot be restored (rc=%d)", restored);
        } else if (restored > 0) {
            kinfo("init: %d configuration file(s) restored from the NVS", restored);
        }

        /*
         * The files of /etc that the model has: a volume of a board that has
         * never been used has none, and a factory reset leaves a store as
         * empty as the first boot. They are written before the save below,
         * so the NVS keeps a copy of them like of any other file.
         */
        defaults = factory_defaults();
        if (defaults > 0) {
            kinfo("init: %d default file(s) of /etc written", defaults);
        } else if (defaults < 0) {
            kwarn("init: the default files of /etc are not written (rc=%d)", defaults);
        }

        /*
         * The systems of parameters (ADR 0021): display, font, console. The
         * same rule as the files of the factory - a file of a person is never
         * replaced - and they are written before the save below, so the NVS
         * keeps them like any other file of /etc.
         */
        defaults = settings_defaults(CONFIG_DIR);
        if (defaults > 0) {
            kinfo("init: %d default setting file(s) of /etc made", defaults);
        } else if (defaults < 0) {
            kwarn("init: the default settings of /etc are not made (rc=%d)", defaults);
        }

        saved = config_persist(CONFIG_DIR);

        if (saved < 0) {
            kwarn("init: the configuration cannot be saved (rc=%d)", saved);
        } else if (saved > 0) {
            kinfo("init: %d configuration file(s) saved to the NVS", saved);
        }
    } else {
        kwarn("init: the configuration has no store");
    }

    /*
     * The systems of /etc are applied in the order of the machine (ADR 0021):
     * the display and the font before the first frame, the console before the
     * shell. The frame itself comes later, when the task of the shell starts
     * the console of the panel.
     */
    (void)settings_load_all();

    /*
     * The other volumes: /etc/fstab is read after the root, whose /etc
     * holds the file. A line that cannot be mounted is named in the log and
     * skipped: the board is useful without the card of the camera, and a
     * boot that stopped for it would not even reach the shell that repairs
     * it.
     */
    mounted = vfs_mount_fstab(VFS_FSTAB_PATH);
    if (mounted < 0) {
        if (mounted != KERR_NOENT) {
            kwarn("init: cannot read the table %s (rc=%d)", VFS_FSTAB_PATH, mounted);
        }
    } else if (mounted > 0) {
        kinfo("init: %d volume(s) mounted from %s", mounted, VFS_FSTAB_PATH);
    }

    /*
     * The commands of the shell have to exist before a script names one:
     * the shell registers them when it starts, and the scripts of the init
     * process run before it.
     */
    if (kshell_init() != KSHELL_OK) {
        kwarn("init: not every command of the shell is registered");
    }

    /*
     * The scripts of /etc/init.d are the daemons of the board: each one
     * runs in a task of its own, so a service that never returns does not
     * hold the init process.
     */
    if (initd_run(INITD_DIR) < 0) {
        kwarn("init: the scripts of %s did not run", INITD_DIR);
    }

    /* And finally the console: a session that starts as root, and another
     * one after every `logout`, which is the way back from a user. */
    console_bring_up();

    if (session_console_start() == 0) {
        kerror("init: cannot start the session of the console");
    }

    for (;;) {
        int status = 0;
        int pid;

        while ((pid = ksys_waitpid(-1, &status, KSYS_WNOHANG)) > 0) {
            session_console_reaped((k_u32_t)pid, status);
        }

        k_logfile_tick();
        k_sched_sleep_ms(100);
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
    const struct kernel_board *info = board_get_info();
    struct kernel_boot_config cfg;

    /*
     * The board has no PMIC: the power latch is kept closed by HOLD (G4),
     * and after a deep sleep the pin comes up floating. Driving it high is
     * the first thing the firmware does, before the console and before the
     * kernel, so a board on a battery does not cut its own power in the
     * first milliseconds of the wake.
     */
    (void)drv_power_hold();

    port_esp32_console_init();
    port_esp32_init();

    /*
     * The image and its version are checked before the filesystem and the
     * shell (ADR 0020). The ROM and the bootloader have already entered this
     * application, so this is the electronic check of the board, not a
     * replacement for secure boot; with the policy off it is a warning.
     *
     * An image of a slot of the update that does not pass is not kept: the
     * slot is marked invalid and the board restarts into the other one
     * (ADR 0022). With no other image to run the call returns and the board
     * stops, which is the answer of the policy.
     *
     * The check comes before klog_capture_start() on purpose: its sentence
     * belongs on the console even when the volume of the board is damaged
     * and the boot log can never be written.
     */
    if (port_esp32_image_check() != 0) {
        kerror("boot: the image did not pass the boot check, the slot is not kept");
        port_esp32_ota_rollback();
        kerror("boot: the board stops");
        k_port_halt();
    }

    /* The log of the boot is collected, it is written to a file later. */
    klog_capture_start();

    /* The time base of the kernel: one tick per millisecond. */
    port_esp32_start_tick(1);

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = info->name;
    cfg.board_desc = info->desc;
    cfg.board_info = info;
    cfg.subsystems = g_subsystems;
    cfg.nsubsystems = K_ARRAY_SIZE(g_subsystems);

    kernel_boot(&cfg);
}
