/*
 * Micro OS — host simulator entry point.
 *
 * Boots the very same kernel core that runs on the device, but on Linux, and
 * then loads the same system: the volume of MicroFS (a file of the machine,
 * see blockdev_file.c), /proc, /dev, the configuration of /etc, the scripts
 * of /etc/init.d and the shell. The board brings these up in main/src/main.c;
 * the simulator follows it level by level, minus the hardware, so that half
 * of the system is exercised on a laptop before it is flashed.
 */
#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "config/config.h"
#include "config/session.h"
#include "config/users.h"
#include "devfs/devfs.h"
#include "graphics/tft_console.h"
#include "init/initd.h"
#include "kernel/boot.h"
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/logfile.h"
#include "kernel/pagecache.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"
#include "net/net.h"
#include "port_host.h"
#include "procfs/procfs.h"
#include "settings/settings.h"

/* The device the volume of the simulator is registered as. */
#define SIM_VOLUME_DEVICE "file0"

/*
 * Whether the volume is mounted. The scripts of /etc/init.d live on it, so
 * without a volume there is nothing for the init process to run; the shell
 * still starts and can format it with `mkfs -y`.
 */
static bool s_volume_mounted;

static void heartbeat_task(void *arg)
{
    K_UNUSED(arg);

    kinfo("heartbeat: started");

    for (;;) {
        kinfo("heartbeat: uptime %llu ms, %u task(s)", (unsigned long long)k_sched_uptime_ms(),
              (unsigned)task_count());
        k_sched_sleep_ms(2000);
    }
}

static void worker_task(void *arg)
{
    int id = (int)(intptr_t)arg;
    int i;

    kinfo("worker %d: started", id);

    for (i = 0; i < 5; i++) {
        kinfo("worker %d: step %d", id, i);
        k_sched_sleep_ms(200);
    }

    kinfo("worker %d: done", id);
}

/*
 * The command that formats the volume of the simulator, the same one the
 * board has. It refuses without the flag -y: the whole content of the volume
 * is destroyed.
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
    s_volume_mounted = false;

    if (mfs_vfs_format(SIM_VOLUME_DEVICE) != 0) {
        kshell_printf("mkfs: the volume cannot be formatted\n");
        return KSHELL_ERROR;
    }

    if (mfs_vfs_mount("/", SIM_VOLUME_DEVICE) != 0) {
        kshell_printf("mkfs: the volume is formatted, but it cannot be mounted\n");
        return KSHELL_ERROR;
    }

    s_volume_mounted = true;
    kshell_printf("mkfs: the volume is formatted\n");

    return KSHELL_OK;
}

/* The volume of the simulator, registered before any filesystem asks for it. */
static int storage_subsys_init(void)
{
    port_host_register_blockdev();

    kinfo("sim: the volume is %s (%u KB)", port_host_volume_path(),
          (unsigned)((MICROOS_VOLUME_SECTORS * PORT_HOST_VOLUME_SECTOR_SIZE) / 1024u));

    return 0;
}

static int fs_subsys_init(void)
{
    int rc;

    /*
     * The command is registered before the mount: when the volume of a run
     * is damaged, the simulator still starts and the shell can format it
     * again, exactly as the board does.
     */
    kshell_register("mkfs", "format the volume: mkfs -y", cmd_mkfs);

    /* MicroFS is a type of the VFS, as it is on the board. */
    if (mfs_vfs_register_type() != 0) {
        kwarn("sim: MicroFS could not register its type");
    }

    rc = mfs_vfs_mount("/", SIM_VOLUME_DEVICE);
    if (rc != 0) {
        /*
         * A missing volume is not a reason to stop: /proc, /dev and the
         * shell do not live on it, and the information about why the mount
         * failed is in the log of the boot.
         */
        kwarn("sim: the volume is not available (rc=%d), /etc is not there", rc);
    } else {
        s_volume_mounted = true;
    }

    /* The information about the system and the devices. */
    if (procfs_register("/proc") != 0) {
        kerror("sim: cannot mount the procfs");
        return -1;
    }

    if (devfs_register("/dev") != 0) {
        kerror("sim: cannot mount the devfs");
        return -1;
    }

    return 0;
}

/*
 * The console of the simulator, once the boot is proven. The board mirrors
 * the shell to the panel; the simulator does the same when a file screen was
 * asked for. Without one there is no renderer and the console of the display
 * stays silent, so it is not started.
 */
static void console_bring_up(void)
{
    if (port_host_screen_enabled()) {
        tft_console_start();
        kshell_set_sink(tft_console_write);
    }
}

/*
 * The board stubs come up before the init process runs the scripts of
 * /etc/init.d and before the shell registers its own commands: a script and
 * a person both have to find `modules`, `date` and the rest already there.
 */
static int stubs_subsys_init(void)
{
    port_host_stubs_init();
    return 0;
}

/*
 * The init process of the simulator: the steps of the init task of the board
 * without the self test and the hardware doctor, which need a chip. The
 * configuration of /etc, the scripts of /etc/init.d, then the shell.
 */
static void init_task(void *arg)
{
    int restored;
    int saved;
    int defaults;
    int mounted;

    K_UNUSED(arg);

    if (k_logfile_start(klog_captured_text(), klog_captured_size(), true) != 0) {
        kerror("sim: the logs cannot be installed");
    }
    klog_capture_stop();

    kinfo("sim: init is up, %u task(s)", (unsigned)task_count());

    if (config_register() != 0) {
        kwarn("sim: the config command is not registered");
    }

    /*
     * The store is installed by config_register(), so the restore below has
     * one: without it /etc would come back empty after a format and the
     * settings of the volume would be lost without a word.
     */
    if (config_store_get() != NULL) {
        restored = config_restore(CONFIG_DIR);

        /* The systems of parameters (ADR 0021) of a volume that has none:
         * the same defaults the board writes, so the simulator has the tree
         * of the machine it imitates. */
        defaults = settings_defaults(CONFIG_DIR);
        if (defaults > 0) {
            kinfo("sim: %d default setting file(s) of /etc made", defaults);
        } else if (defaults < 0) {
            kwarn("sim: the default settings of /etc are not made (rc=%d)", defaults);
        }

        /* The accounts of /etc (ADR 0033): the two files and the home of an
         * account, when the volume has none. They are made without a line of
         * their own: the ring of the log is short and the count of the files
         * saved to the store below says that they are there. */
        defaults = users_defaults(CONFIG_DIR);
        if (defaults < 0) {
            kwarn("sim: the accounts of /etc are not made (rc=%d)", defaults);
        }

        saved = config_persist(CONFIG_DIR);

        if (restored < 0) {
            kwarn("sim: the configuration cannot be restored (rc=%d)", restored);
        } else if (restored > 0) {
            kinfo("sim: %d configuration file(s) restored from the store", restored);
        }

        if (saved < 0) {
            kwarn("sim: the configuration cannot be saved (rc=%d)", saved);
        } else if (saved > 0) {
            kinfo("sim: %d configuration file(s) saved to the store", saved);
        }
    } else {
        kwarn("sim: the configuration has no store");
    }

    /*
     * The systems of /etc (ADR 0021): the host has no screen of its own, so
     * the load names the systems it cannot apply and keeps their files, which
     * is what the ADR asks of a machine without the hardware. A run with a
     * screen file turns the console of the display on below.
     */
    (void)settings_load_all();

    /*
     * The volumes of the table of /etc/fstab: the root is mounted, so the
     * file has a place. A line that does not mount is in the log and is
     * skipped, the same way the board does it.
     */
    if (s_volume_mounted) {
        mounted = vfs_mount_fstab(VFS_FSTAB_PATH);

        if (mounted < 0) {
            if (mounted != KERR_NOENT) {
                kwarn("sim: cannot read the table %s (rc=%d)", VFS_FSTAB_PATH, mounted);
            }
        } else if (mounted > 0) {
            kinfo("sim: %d volume(s) mounted from %s", mounted, VFS_FSTAB_PATH);
        }
    }

    /*
     * The commands of the shell have to exist before a script names one: the
     * shell registers them when it starts, and the scripts of the init
     * process run before it.
     */
    if (kshell_init() != KSHELL_OK) {
        kwarn("sim: not every command of the shell is registered");
    }

#if MICRO_OS_NET
    /* The commands of the network: the HTTP client, the clock from NTP and
     * the page of the state. The host port has the sockets of the process,
     * so the same sources the board takes to lwIP work over the loopback. */
    if (net_register() != 0) {
        kwarn("sim: not every command of the network is registered");
    }
#endif

    if (s_volume_mounted && initd_run(INITD_DIR) < 0) {
        kwarn("sim: the scripts of %s did not run", INITD_DIR);
    }

    /*
     * The console: a session that starts as root, and another one after every
     * `logout`. The init of the board does the same; here the loop is also
     * the reaper of the tasks a person starts from the shell, so the table of
     * 32 slots does not fill with zombies.
     */
    console_bring_up();

    if (session_console_start() == 0) {
        kerror("sim: cannot start the session of the console");
    }

    for (;;) {
        int status = 0;
        int pid;

        while ((pid = ksys_waitpid(-1, &status, KSYS_WNOHANG)) > 0) {
            session_console_reaped((k_u32_t)pid, status);
        }

        k_logfile_tick();
        pagecache_tick();
        k_sched_sleep_ms(100);
    }
}

static int tasks_subsys_init(void)
{
    /* The load of the scheduler: a heartbeat and two workers that finish. */
    if (task_create("heartbeat", heartbeat_task, NULL, 8) == NULL) {
        return -1;
    }

    if (task_create("worker0", worker_task, (void *)(intptr_t)0, 5) == NULL) {
        return -1;
    }

    if (task_create("worker1", worker_task, (void *)(intptr_t)1, 5) == NULL) {
        return -1;
    }

    /* And the first task of the system, the one that starts the rest. */
    if (task_create("init", init_task, NULL, TASK_PRIO_DEFAULT) == NULL) {
        return -1;
    }

    return 0;
}

static const struct kernel_subsys g_subsystems[] = {
    {"storage", storage_subsys_init, KERNEL_SUBSYS_LEVEL_CORE},
    {"fs", fs_subsys_init, KERNEL_SUBSYS_LEVEL_SERVICE},
    {"stubs", stubs_subsys_init, KERNEL_SUBSYS_LEVEL_SERVICE},
    {"tasks", tasks_subsys_init, KERNEL_SUBSYS_LEVEL_APP},
};

int main(void)
{
    const struct kernel_board *info = board_get_info();
    struct kernel_boot_config cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = info->name;
    cfg.board_desc = info->desc;
    cfg.board_info = info;
    cfg.subsystems = g_subsystems;
    cfg.nsubsystems = K_ARRAY_SIZE(g_subsystems);

    /* The console of the host exists from the first line, so the capture is
     * mirrored while the root is still being mounted. */
    klog_capture_start_mirroring();

    /* The time base of the kernel: one tick per millisecond. */
    port_host_start_tick(1);

    kernel_boot(&cfg);

    /* The shell task owns the console, the main thread waits for it. */
    for (;;) {
        k_sched_sleep_ms(1000);
    }
}
