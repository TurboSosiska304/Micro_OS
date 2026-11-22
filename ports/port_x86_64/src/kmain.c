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
 *
 * Between the banner and the init process the boot brings up the filesystem
 * of the machine and runs a program out of it (see the section at the end of
 * this file): this is the machine of the project that has the memory to place
 * an image and the processor to jump into it, so a program of the phase is a
 * program that really runs, out of a file of the disk, and not a figure of
 * speech.
 */
#include "port_x86_64.h"

#include <string.h>

#include "kernel/board.h"
#include "kernel/boot.h"
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/vfs.h"

#include "config/config.h"
#include "kshell/kshell.h"
#include "port_x86_64_console.h"
#include "port_x86_64_screen.h"
#include "settings/settings.h"

#if MICRO_OS_NET
#include "net/net.h"
#endif

#if MICRO_OS_PKG
#include "pkg/pkg.h"
#endif

#include "devfs/devfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"
#include "procfs/procfs.h"
#if MICRO_OS_FS_FAT32
#include "fs_fat32/fat.h"
#endif
#if defined(MICRO_OS_FS_EXT2) && MICRO_OS_FS_EXT2
#include "fs_ext2/ext2.h"
#endif

/* The body of the shell task, in src/shell.c. */
void port_x86_64_shell_run(void);

/* The volume the boot mounts: the first partition of the disk of QEMU when
 * one was attached, "ram0" otherwise (src/ata.c, ADR 0008). */
const char *port_x86_64_root_device(void);

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

/* --- the filesystem and the program of the boot -------------------------- */

/*
 * The programs are files of the disk, not blobs of the kernel. The image of
 * the machine carries a MicroFS volume (tools/mkfs_mfs.c, `make x86-disk`),
 * the boot mounts it at `/` and finds `/bin` where the tool left it; the
 * file is then reached the way any caller reaches it: through execve.
 *
 * A boot that reads the image and jumps into it would prove the port and the
 * parser, and nothing of the edge of the system: the stat of the file, the
 * bit of execution in its mode, the read through the VFS, the memory of the
 * kernel and the call itself are exactly what a program from /bin has to go
 * through, and the line of the program on COM1 is the proof that all of it
 * happened in that order.
 *
 * A machine without a disk still mounts a volume in its memory (ram0, ADR
 * 0008); that volume has no /bin, and the boot says which file it could not
 * find instead of pretending to have one.
 */
#define PROGRAM_PATH "/bin/hello"

/*
 * Run the file through the call. The call is the whole of the path: the stat,
 * the check of the mode, the read through the VFS into the memory of the
 * kernel, the parse of the image and the jump of the port. The boot does not
 * repeat any of it.
 *
 * The call comes back because this program ends with the call of exit and the
 * port takes the task down; the kernel answers every end of a program with 0,
 * which says "the program ran and has finished", and the status the program
 * ended with is the line the port printed before this one (ADR 0006).
 */
static void program_run(void)
{
    int rc;

    kinfo("x86-64: running %s through the call", PROGRAM_PATH);
    rc = ksys_execve(PROGRAM_PATH, NULL);

    if (rc == 0) {
        kinfo("x86-64: the program returned");
        return;
    }

    if (rc == KERR_NOSYS) {
        kerror("x86-64: this machine cannot run a program");
        return;
    }

    /* A volume without the file is a machine without a userland: the boot
     * says so once and goes on to the shell. The disks of the tests carry
     * /bin, and the ones of a person may not. */
    if (rc == KERR_NOENT) {
        kwarn("x86-64: there is no %s on %s, no program at the boot", PROGRAM_PATH,
              port_x86_64_root_device());
        return;
    }

    /* The one refusal that is about the file and not about the medium: the
     * call answered EPERM, so the mode of /bin/hello lost its `x` bits. */
    if (rc == KERR_PERM) {
        kerror("x86-64: %s is not executable", PROGRAM_PATH);
        return;
    }

    kerror("x86-64: %s did not run (%s, %d)", PROGRAM_PATH, vfs_strerror(rc), rc);
}

/*
 * The tree a machine of this system has: /etc with the file of the settings
 * and the directory of the init scripts. The image of the disk carries them
 * (`make x86-disk`), so a boot on that image writes nothing; a volume
 * without them - the RAM volume of a machine QEMU was given no disk for -
 * gets them here, and the tree of x86-64 is the tree of the board and of the
 * simulator. A setting that is already there is never replaced: it belongs to
 * the person who wrote it.
 */
#define ETC_DIR "/etc"
#define ETC_INITD "/etc/init.d"
#define ETC_SETTING "/etc/setting"
#define ETC_SETTING_TEXT "hello\nhostname=x86_64\nconsole=vga\n"

static void etc_bring_up(void)
{
    struct vfs_stat info;
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    const k_size_t size = (k_size_t)strlen(ETC_SETTING_TEXT);
    int rc;

    rc = vfs_stat(ETC_DIR, &info);
    if (rc != 0) {
        if (rc != KERR_NOENT) {
            kwarn("x86-64: cannot look at %s (%s)", ETC_DIR, vfs_strerror(rc));
            return;
        }

        rc = vfs_mkdir_p(ETC_DIR);
        if (rc != 0) {
            kwarn("x86-64: cannot make %s (%s)", ETC_DIR, vfs_strerror(rc));
            return;
        }

        kinfo("x86-64: %s is made on the volume", ETC_DIR);
    }

    rc = vfs_mkdir_p(ETC_INITD);
    if (rc != 0) {
        kwarn("x86-64: cannot make %s (%s)", ETC_INITD, vfs_strerror(rc));
    }

    rc = vfs_stat(ETC_SETTING, &info);
    if (rc == 0) {
        return;
    }

    if (rc != KERR_NOENT) {
        kwarn("x86-64: cannot look at %s (%s)", ETC_SETTING, vfs_strerror(rc));
        return;
    }

    rc = vfs_open(ETC_SETTING, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    if (rc != 0) {
        kwarn("x86-64: cannot write %s (%s)", ETC_SETTING, vfs_strerror(rc));
        return;
    }

    rc = vfs_write(&file, ETC_SETTING_TEXT, size);
    (void)vfs_close(&file);

    if (rc != (int)size) {
        kwarn("x86-64: %s took %d byte(s) of %u", ETC_SETTING, rc, (unsigned)size);
        return;
    }

    (void)vfs_chmod(ETC_SETTING, 0644u);
    kinfo("x86-64: %s is made on the volume", ETC_SETTING);
}

/*
 * The filesystem of the machine: the disk of the port when QEMU was given one
 * and the first partition of it (ADR 0008), otherwise a volume of the memory
 * of the port mounted at the root. The device is registered by
 * port_x86_64_init() before the kernel boots, and a volume that was never
 * formatted is formatted here (mfs_vfs_mount does that; a fresh disk reads as
 * a blank medium, see blockdev_ram.c and ata.c).
 */
static void filesystem_bring_up(void)
{
    const char *device = port_x86_64_root_device();
    int rc;

#if MICRO_OS_FS_FAT32
    if (fat_vfs_register_type() != 0) {
        kwarn("x86-64: the FAT filesystem could not register its type");
    }
#endif

#if defined(MICRO_OS_FS_EXT2) && MICRO_OS_FS_EXT2
    if (ext2_vfs_register_type() != 0) {
        kwarn("x86-64: the ext2 filesystem could not register its type");
    }
#endif

    /* MicroFS is a type as well, so that /etc/fstab and the shell can name
     * the volume of the machine like any other volume. */
    if (mfs_vfs_register_type() != 0) {
        kwarn("x86-64: MicroFS could not register its type");
    }

    rc = mfs_vfs_mount("/", device);

    if (rc != 0) {
        kerror("x86-64: cannot mount %s at / (%s), there is no /bin", device, mfs_strerror(rc));
        return;
    }

    /* The tree of the machine: /etc of the volume or of the memory, then the
     * two virtual filesystems the board and the simulator mount. Their
     * failure costs a directory and not the boot. */
    etc_bring_up();

    if (procfs_register("/proc") != 0) {
        kwarn("x86-64: cannot mount the procfs at /proc");
    }

    if (devfs_register("/dev") != 0) {
        kwarn("x86-64: cannot mount the devfs at /dev");
    }

    /* The programs are files the tool left in the volume; nothing is
     * written here. A volume a person made without /bin is a machine
     * without programs, and the boot says so when it asks for one. */
    kinfo("x86-64: the programs come from %s:/bin", device);
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

    /*
     * The program of the boot runs before the shell: execve is a wait now,
     * so the init task is its parent and the shell does not exist until the
     * program has ended. That is the order of the transcript the smoke test
     * reads: the lines of /bin/hello, then the prompt.
     */
    program_run();

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
    int mounted;
    int defaults;

    port_x86_64_init(multiboot_info);

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = g_board.name;
    cfg.board_desc = g_board.desc;
    cfg.board_info = &g_board;

    kernel_boot(&cfg);

    kinfo("x86-64: the kernel is up, heap %u KB free of %u KB",
          (unsigned)(k_port_heap_free() / 1024), (unsigned)(k_port_heap_total() / 1024));

    /* The screen the boot chose (ADR 0023): the framebuffer of the VGA and
     * its grid of cells, or the text mode of the firmware when the machine
     * has no interface to program one. The serial port carries the line
     * either way, which is how a machine without a window is read. */
    kinfo("x86-64: the screen is %s", port_x86_64_console_name());

    /*
     * The processors of the machine (ADR 0012). The count on the left is what
     * the firmware (the MADT of ACPI) says, the one on the right is what the
     * kernel runs: the APs need the trampoline, INIT/SIPI, a local APIC timer
     * and an I/O APIC, and this port does not have them yet. Saying both
     * numbers is the honest line; putting a task in the queue of a core that
     * never runs it would hang the machine instead.
     */
    kinfo("x86-64: %u cpu(s) reported by the firmware, %u in use (apic 0x%x)",
          (unsigned)port_x86_64_cpu_available(), (unsigned)port_x86_64_cpu_started(),
          (unsigned)port_x86_64_apic_base());

    if (port_x86_64_cpu_available() > port_x86_64_cpu_started()) {
        kwarn("x86-64: the AP bring-up is not written yet, the kernel runs on the core 0");
    }

    /*
     * The tables are read back before a program is ever entered: the port
     * promises that no page is writable and executable at once, and a map
     * that says otherwise is a bug of the port that the boot names instead of
     * running with. The audit walks the same tables the processor walks
     * (ADR 0007, NX and W^X).
     */
    if (port_x86_64_paging_audit(port_x86_64_kernel_cr3()) == 0) {
        kinfo("x86-64: the map is W^X, the text is read-only and the data is NX");
    } else {
        kerror("x86-64: the map is not W^X, a page is writable and executable");
    }

    /*
     * The filesystem, before the tasks: the volume is mounted and the images
     * of the programs are written to /bin. The program itself runs later, in
     * the init task: execve is a wait for a task now, and the boot context
     * has no scheduler under it to wait with.
     */
    filesystem_bring_up();

    /*
     * The settings of /etc (ADR 0021): the systems of the tree, and the ones
     * this machine applies. `console` is the pair of the screen of the port,
     * `font` is the gap between two of its lines; `display` is the panel of
     * the board, and the load names it and keeps the file. The command
     * `config` is registered before the shell exists, so the first line a
     * person types finds it.
     */
    if (config_register() != 0) {
        kwarn("x86-64: the config command is not registered");
    }
    if (port_x86_64_console_settings_register() != SETTINGS_OK) {
        kwarn("x86-64: the console system of /etc is not registered");
    }
    if (port_x86_64_font_settings_register() != SETTINGS_OK) {
        kwarn("x86-64: the font system of /etc is not registered");
    }

    defaults = settings_defaults(SETTINGS_DIR);
    if (defaults < 0) {
        kwarn("x86-64: the default settings of /etc are not made");
    } else if (defaults > 0) {
        kinfo("x86-64: %d default setting file(s) of /etc made", defaults);
    }

    (void)settings_load_all();

    /*
     * The first frame of the picture is drawn twice. The switch from the text
     * mode of the firmware to the framebuffer is a moment when a display may
     * still hold the screen it drew before it, and the writes of the boot
     * cover only the lines that exist; the second pass puts every pixel of
     * the picture - the log that is already in the cells - through the
     * display once more, so no line of the old 80x25 screen is left under the
     * new one. The theme of /etc has been applied just above, so the pass
     * carries the colours a person chose.
     */
    port_x86_64_console_redraw();

    /* The commands of the network, before the shell exists: `netinfo`,
     * `ping` and `netudp` come from the port, and the client of HTTP and the
     * clock of NTP from components/net when the image has them. */
    if (port_x86_64_net_register() != 0) {
        kwarn("x86-64: not every command of the network is registered");
    }

#if MICRO_OS_NET
    if (net_register() != 0) {
        kwarn("x86-64: not every command of components/net is registered");
    }
#endif

#if MICRO_OS_PKG
    /* The installer of programs (ADR 0024): the tree of /usr/bin, /var and
     * the network of the machine, which is the same socket the clients use. */
    if (pkg_register() != 0) {
        kwarn("x86-64: the pkg command is not registered");
    }
#endif

    /*
     * The other volumes: /etc/fstab, when the disk of the machine carries
     * one. The root is the first mount, so the file has a place to live. A
     * line that does not mount is in the log and the boot goes on - the
     * machine has no config store of its own, so the table is exactly what
     * the volume keeps.
     */
    mounted = vfs_mount_fstab(VFS_FSTAB_PATH);
    if (mounted < 0) {
        if (mounted != KERR_NOENT) {
            kwarn("x86-64: cannot read the table %s (rc=%d)", VFS_FSTAB_PATH, mounted);
        }
    } else if (mounted > 0) {
        kinfo("x86-64: %d volume(s) mounted from %s", mounted, VFS_FSTAB_PATH);
    }

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
