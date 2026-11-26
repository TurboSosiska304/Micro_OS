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
#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/logfile.h"
#include "kernel/port.h"
#include "kernel/port/cpu.h"
#include "kernel/root.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/vfs.h"

#include "config/config.h"
#include "config/session.h"
#include "kshell/kshell.h"
#include "port_x86_64_console.h"
#include "port_x86_64_input.h"
#include "port_x86_64_screen.h"
#include "port_x86_64_task.h"
#include "port_x86_64_smp.h"
#include "settings/settings.h"

#if MICRO_OS_NET
#include "net/net.h"
#endif

#if MICRO_OS_PKG
#include "pkg/pkg.h"
#endif

#if MICRO_OS_TELNET
#include "telnet/telnet.h"
#endif

#include "devfs/devfs.h"
#include "mfs/mfs_vfs.h"
#include "procfs/procfs.h"
#if MICRO_OS_FS_FAT32
#include "fs_fat32/fat.h"
#endif
#if defined(MICRO_OS_FS_EXT2) && MICRO_OS_FS_EXT2
#include "fs_ext2/ext2.h"
#endif
#if defined(MICRO_OS_FS_RAMFS) && MICRO_OS_FS_RAMFS
#include "ramfs/ramfs_vfs.h"
#endif

/* The volume the boot mounts: the first partition of the disk of QEMU when
 * one was attached, "ram0" otherwise (src/ata.c, ADR 0008). */
const char *port_x86_64_root_device(void);

/* The nap of the tasks of the port that have nothing to do right now. */
#define PORT_IDLE_MS 1000u

static const struct kernel_board g_board = {
    .name = "x86_64",
    .desc = "the x86-64 machine of this port",
    .soc = "x86-64 with a 8259 or an I/O APIC",
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
 * port_x86_64_init() before the kernel boots, and its type is the business of
 * the kernel (ADR 0027): the port says where the root is, k_root_mount() reads
 * the bytes of the medium and mounts the volume as what it is. A volume that
 * was never formatted is formatted by MicroFS (mfs_vfs_mount does that; a
 * fresh disk reads as a blank medium, see blockdev_ram.c and ata.c).
 *
 * Returns 0 when the root is mounted, a negative code when there is no root.
 * A machine without one has no /bin, no /etc and no /proc, and the caller
 * stops the boot and says why instead of showing a shell over an empty tree.
 */
static int filesystem_bring_up(const char **type)
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
     * the volume of the machine like any other volume. The order of the two
     * registrations is the order of ADR 0027 for a volume two types claim. */
    if (mfs_vfs_register_type() != 0) {
        kwarn("x86-64: MicroFS could not register its type");
    }

#if defined(MICRO_OS_FS_RAMFS) && MICRO_OS_FS_RAMFS
    if (ramfs_vfs_register_type() != 0) {
        kwarn("x86-64: the ramfs could not register its type");
    }
#endif

    rc = k_root_mount(device, type);

    if (rc != 0) {
        kerror("x86-64: the machine has no root filesystem on %s, there is no /bin", device);
        return rc;
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

    /*
     * The two devices of the interface (ADR 0028): the framebuffer a program
     * draws its picture in and the keyboard it reads as events. A machine
     * whose screen is the text mode of the VGA has no /dev/fb0; the input of
     * the keyboard is there because the controller is.
     */
    (void)port_x86_64_fbdev_register();
    port_x86_64_input_register();

#if defined(MICRO_OS_FS_RAMFS) && MICRO_OS_FS_RAMFS
    /*
     * A read-only root cannot hold the files of the desk in /tmp: the
     * window manager keeps the pixels of a window there, and the stick and
     * the ISO are read-only by the rule of ADR 0041. Mount a ramfs at /tmp
     * so that the machine of 2.0 can draw its windows on the medium the
     * release hands out.
     */
    {
        k_size_t i;
        bool root_ro = false;

        for (i = 0; i < vfs_mount_count(); i++) {
            if (strcmp(vfs_mount_name(i), "/") == 0) {
                root_ro = vfs_mount_is_read_only(i);
                break;
            }
        }

        if (root_ro) {
            int trc = vfs_mount_type("/tmp", "ramfs", NULL);
            if (trc == 0) {
                kinfo("x86-64: /tmp is a ramfs, the root is read-only");
            } else {
                kwarn("x86-64: /tmp could not be mounted as ramfs (%s)", vfs_strerror(trc));
            }
        }
    }
#endif

    /* The programs are files the tool left in the volume; nothing is
     * written here. A volume a person made without /bin is a machine
     * without programs, and the boot says so when it asks for one. */
    kinfo("x86-64: the programs come from %s:/bin, the volume is %s", device, *type);

    return 0;
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

    /* The root is mounted and this task can block on the VFS. Install the
     * captured boot once; everything printed from here belongs to run.log. */
    if (k_logfile_start(klog_captured_text(), klog_captured_size(), true) != 0) {
        kerror("x86-64: the logs cannot be installed");
    }
    klog_capture_stop();

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

    /*
     * The colours of the log of the session, from the first prompt: a warning
     * is amber, an error is red and a success is green, and the screen the
     * machine has - the framebuffer or the sixteen attributes of the VGA -
     * reads the SGR sequences (kernel/ansi.h). The log of the boot above is
     * made of the plain pair of /etc, so a screenshot of the boot is the two
     * colours a person chose and nothing else; COM1 is plain either way,
     * because the port strips the sequences from its copy (src/port_x86_64.c).
     */
    k_console_set_ansi(true);

    /*
     * The console session: a task of the kernel with the UART for a terminal,
     * which starts as root and asks nothing - the person in front of the
     * machine is the machine (ADR 0033). `logout` ends the task and this
     * init, which is the getty of the port, starts another one as root.
     */
    if (session_console_start() == 0) {
        kerror("x86-64: cannot start the session of the console");
    }

#if MICRO_OS_TELNET
    /*
     * The shell over the air, with the accounts of /etc (ADR 0033). It is
     * registered here and not in kmain: the task of the listener is the
     * second task the machine makes and the boot context hands the CPU to
     * the first one it creates (k_port_task_start), so a task created before
     * the init would have left the machine without an init at all. The
     * order puts the session of the console at pid 3, where the smoke test
     * of this port knows it.
     */
    if (telnet_register() != 0) {
        kwarn("x86-64: the telnet server is not registered");
    }
#endif

    /*
     * The init of the machine is also the parent of the last resort: a child
     * whose parent died is reparented to pid 1 by the kernel, and this loop
     * is what reaps it. A zombie that nobody waits for keeps its slot of the
     * table of 32, and a console that forks programs in a loop (the soak of
     * the port) would fill the table. The interval is a tenth of a second:
     * the wait of a finite timeout costs nothing and the slot is back before
     * a person or a test can see it. A child that was the console and left
     * with a logout is a session to start again.
     */
    for (;;) {
        int status = 0;
        int pid;

        while ((pid = ksys_waitpid(-1, &status, KSYS_WNOHANG)) > 0) {
            session_console_reaped((k_u32_t)pid, status);
        }

        /* The existing init loop owns the log batches: no extra task slot and
         * no writer left running when this task is signalled at shutdown. */
        k_logfile_tick();
        k_sched_sleep_ms(100u);
    }
}

void kmain(k_u64_t boot_info, k_u64_t boot_magic, k_u64_t kernel_phys)
{
    struct kernel_boot_config cfg;
    const char *root_type = NULL;
    int mounted;
    int defaults;

    /* COM1 exists before the kernel does. Keep the first boot line in the
     * capture without hiding it from the person watching the console. */
    klog_capture_start_mirroring();

    port_x86_64_init(boot_info, boot_magic, kernel_phys);

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = g_board.name;
    cfg.board_desc = g_board.desc;
    cfg.board_info = &g_board;

    kernel_boot(&cfg);

    /* Which of the two boots of this port it is: the multiboot image that
     * QEMU loads with -kernel, or the machine the firmware handed over
     * (loader/uefi). The rest of the log is the same on both, which is the
     * point of the port: one kernel, two ways of starting it. */
    kinfo("x86-64: the boot is %s, the image at 0x%x", port_x86_64_boot_name(),
          (unsigned)g_port_x86_64_kernel_phys);

    /*
     * The cores that stopped before the scheduler may enter it now: the
     * array of kernel/cpu.c exists (k_sched_init ran inside kernel_boot) and
     * its count is the number of cores that reached C. The release comes
     * before the first task_create(), because a core that entered the
     * scheduler has to find the queues it is going to share.
     */
    port_x86_64_smp_release();

    kinfo("x86-64: the kernel is up, heap %u KB free of %u KB",
          (unsigned)(k_port_heap_free() / 1024), (unsigned)(k_port_heap_total() / 1024));

    /*
     * The keyboard of the machine and the set of its scancodes: the port asks
     * the device and the controller of it at the boot (src/ps2.c), and the two
     * numbers are the first thing a person with a laptop of the set 2 wants to
     * see. A device that did not answer is the set 1 of every machine of the
     * port, and the line says that too.
     */
    if (port_x86_64_ps2_set_ordered()) {
        /*
         * The device was told to speak the set 1 and it answered: the wire is
         * the set 1 whatever the controller claims, which is the one case a
         * laptop has needed.
         */
        kinfo("ps2: the keyboard was told to speak set 1, and it does");
    } else if (port_x86_64_ps2_device_set() == 0u) {
        kinfo("ps2: the keyboard did not say which scancode set it speaks, set %u is assumed",
              (unsigned)port_x86_64_ps2_set());
    } else if (port_x86_64_ps2_device_set() == port_x86_64_ps2_set()) {
        kinfo("ps2: the keyboard speaks set %u", (unsigned)port_x86_64_ps2_set());
    } else {
        kinfo("ps2: the keyboard speaks set %u, the controller translates it into set %u",
              (unsigned)port_x86_64_ps2_device_set(), (unsigned)port_x86_64_ps2_set());
    }

    /* The screen the boot chose (ADR 0023): the framebuffer of the VGA and
     * its grid of cells, or the text mode of the firmware when the machine
     * has no interface to program one. The serial port carries the line
     * either way, which is how a machine without a window is read. */
    kinfo("x86-64: the screen is %s", port_x86_64_console_name());

    /*
     * The processors of the machine (ADR 0012). The count on the left is what
     * the firmware (the MADT of ACPI) says, the one on the right is what the
     * kernel runs: a core counts here only when it reached C and was accepted
     * by the boot. Saying both numbers is the honest line; putting a task in
     * the queue of a core that never runs it would hang the machine instead.
     */
    kinfo("x86-64: %u cpu(s) reported by the firmware, %u in use (apic 0x%x)",
          (unsigned)port_x86_64_cpu_available(), (unsigned)port_x86_64_cpu_started(),
          (unsigned)port_x86_64_apic_base());

#if KERNEL_SMP
    if (!port_x86_64_smp_apic_ready()) {
        kwarn("x86-64: no local APIC, the machine runs on the core 0");
    } else if (port_x86_64_cpu_started() < port_x86_64_cpu_available()) {
        kwarn("x86-64: %u core(s) of the firmware did not answer the start-up IPI",
              (unsigned)(port_x86_64_cpu_available() - port_x86_64_cpu_started()));
    }
#else
    /* A one-core build does not start an AP whatever the firmware lists: the
     * flag is the promise of 1.2 behaviour, and the line says so instead of
     * blaming an APIC that the machine may well have. */
    kinfo("x86-64: this kernel is built without KERNEL_SMP, the core 0 runs alone");
#endif

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
     * The two bits of CR4 that keep the kernel out of the memory of a program
     * (ADR 0052). SMEP refuses a fetch of a page of ring 3 from ring 0 and
     * SMAP a read or a write of one; both are turned on when the processor
     * advertises them and named here either way, because a machine that has
     * neither is protected by the supervisor bit of every kernel page and
     * that is the line a person reads when an attack does not work.
     */
    if (port_x86_64_smep_on() && port_x86_64_smap_on()) {
        kinfo("x86-64: SMEP and SMAP are on, the kernel cannot run or read a page of a program");
    } else if (port_x86_64_smep_on()) {
        kinfo("x86-64: SMEP is on, SMAP is not on this processor");
    } else if (port_x86_64_smap_on()) {
        kinfo("x86-64: SMAP is on, SMEP is not on this processor");
    } else {
        kinfo("x86-64: this processor has no SMEP or SMAP, the supervisor bit of every kernel "
              "page is the boundary");
    }

    /*
     * The filesystem, before the tasks: the type of the volume is read from
     * the medium and the volume is mounted with it (ADR 0027), and the
     * images of the programs are files of it. The program itself runs later,
     * in the init task: execve is a wait for a task now, and the boot context
     * has no scheduler under it to wait with.
     *
     * A machine without a root has nothing to boot: no /bin, no /etc, no
     * programs and no settings. The sentence is on the console and the boot
     * stops, because a shell over an empty tree would look like a machine
     * whose disk lost its files.
     */
    if (filesystem_bring_up(&root_type) != 0) {
        kerror("x86-64: the boot stops, there is no root filesystem to run from");
        port_x86_64_idle();
    }

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

    /* The proof of the guard page under a kernel stack (ADR 0048): `guard`
     * starts a task that overflows its own stack and ends there. */
    if (port_x86_64_stack_register() != 0) {
        kwarn("x86-64: the guard command is not registered");
    }

#if KERNEL_SMP
    /* The cores of the machine: the state of the scheduler of every core, a
     * run of workers and the IPI of one core to another (ADR 0012). A
     * one-core build has no second core to name and no command. */
    if (port_x86_64_smp_register() != 0) {
        kwarn("x86-64: the smp command is not registered");
    }
#endif

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
         * A machine without a task cannot switch: the boot says so and keeps
         * the core looking at its queue, which is the idle loop of a port
         * whose scheduler exists and has nothing to run.
         */
        kinfo("x86-64: no tasks yet");
        port_x86_64_idle();
    }

    /*
     * The init process owns the rest of the boot: the shell comes from it.
     * The context of kmain is the idle context of the core 0: it looks at the
     * queue and waits, and it is where the port returns when the last task of
     * this core ends. It is reached again only if a task ends and gives the
     * core back, so the return of task_create() above is the normal way here.
     */
    port_x86_64_idle();
}
