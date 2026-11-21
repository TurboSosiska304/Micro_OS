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
 * Between the banner and the init process the boot brings up a filesystem and
 * runs a program out of it (see the section at the end of this file): this is
 * the machine of the project that has the memory to place an image and the
 * processor to jump into it, so a program of the phase is a program that
 * really runs, out of a file, and not a figure of speech.
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

#include "kshell/kshell.h"

#if MICRO_OS_NET
#include "net/net.h"
#endif

#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"
#if MICRO_OS_FS_FAT32
#include "fs_fat32/fat.h"
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

/* --- the filesystem and the program of the milestone --------------------- */

/*
 * The program of the phase is a file, and the boot reaches it the way any
 * caller does: through the call. The image is built at the build time out of
 * programs/hello.c and embedded in the kernel (see the CMakeLists of the
 * port), because the kernel has to have the bytes before it has anywhere to
 * put them; at the boot they are written to /bin/hello of a volume of the
 * memory of the port, and then the boot asks the system for that file with
 * execve.
 *
 * The shape is deliberate. A boot that read the image and jumped into it would
 * prove the port and the parser, and nothing of the edge of the system: the
 * stat of the file, the bit of execution in its mode, the read through the
 * VFS, the memory of the kernel and the call itself are exactly what a program
 * from /bin has to go through, and the line of the program on COM1 is the proof
 * that all of it happened in that order.
 */
extern const k_u8_t _binary_hello_mox_start[];
extern const k_u8_t _binary_hello_mox_end[];
extern const k_u8_t _binary_crash_mox_start[];
extern const k_u8_t _binary_crash_mox_end[];
extern const k_u8_t _binary_fork_mox_start[];
extern const k_u8_t _binary_fork_mox_end[];
extern const k_u8_t _binary_badptr_mox_start[];
extern const k_u8_t _binary_badptr_mox_end[];

#define PROGRAM_DIR "/bin"
#define PROGRAM_PATH "/bin/hello"
#define PROGRAM_CRASH_PATH "/bin/crash"
#define PROGRAM_FORK_PATH "/bin/fork"
#define PROGRAM_BADPTR_PATH "/bin/badptr"

/*
 * A program is a file somebody may run, so it is created with the mode of a
 * program and not with the 0644 of a file of data: without the three bits of
 * execution the call answers EPERM, and an image that cannot be run proves
 * nothing about the machine that was supposed to run it.
 */
#define PROGRAM_MODE 0755u

/*
 * Write an image to a file of /bin. The file is created through the VFS and
 * not through the filesystem: the adapter of MicroFS is the only place that
 * knows which numbers of the filesystem mean what, and the boot is not that
 * place.
 */
static bool program_install(const char *path, const k_u8_t *image, k_size_t size)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    int rc;

    rc = vfs_mkdir_p(PROGRAM_DIR);
    if (rc != 0) {
        kerror("x86-64: cannot make %s (%s)", PROGRAM_DIR, vfs_strerror(rc));
        return false;
    }

    rc = vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    if (rc != 0) {
        kerror("x86-64: cannot create %s (%s)", path, vfs_strerror(rc));
        return false;
    }

    rc = vfs_write(&file, image, size);
    (void)vfs_close(&file);

    if (rc != (int)size) {
        kerror("x86-64: %s took %d byte(s) of %u", path, rc, (unsigned)size);
        return false;
    }

    /* The mode is the second half of the file: a file of data is written and
     * read, a program is written and executed, and the difference is here. */
    rc = vfs_chmod(path, PROGRAM_MODE);
    if (rc != 0) {
        kerror("x86-64: cannot make %s executable (%s)", path, vfs_strerror(rc));
        return false;
    }

    kinfo("x86-64: %s is written (%u bytes)", path, (unsigned)size);

    return true;
}

/*
 * Run the file through the call. The call is the whole of the path: the stat,
 * the check of the mode, the read through the VFS into the memory of the
 * kernel, the parse of the image and the jump of the port. The boot does not
 * repeat any of it.
 *
 * The call comes back only because this program ends with a ret and the port
 * owns the exit; the kernel answers every return with 0, which says "the
 * program ran and has finished", and the status the program ended with is the
 * line the port printed before this one (ADR 0006).
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

    /* The one refusal that is about the file and not about the medium: the
     * call answered EPERM, so the mode of /bin/hello lost its `x` bits. */
    if (rc == KERR_PERM) {
        kerror("x86-64: %s is not executable", PROGRAM_PATH);
        return;
    }

    kerror("x86-64: %s did not run (%s, %d)", PROGRAM_PATH, vfs_strerror(rc), rc);
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

    rc = mfs_vfs_mount("/", device);

    if (rc != 0) {
        kerror("x86-64: cannot mount %s at / (%s), there is no /bin", device, mfs_strerror(rc));
        return;
    }

    if (!program_install(PROGRAM_PATH, _binary_hello_mox_start,
                         (k_size_t)(_binary_hello_mox_end - _binary_hello_mox_start))) {
        return;
    }

    /*
     * The second program exists for the smoke test of the fault path: the
     * shell runs it after the first one, it writes to address zero and the
     * machine has to be alive for the command after it.
     */
    if (!program_install(PROGRAM_CRASH_PATH, _binary_crash_mox_start,
                         (k_size_t)(_binary_crash_mox_end - _binary_crash_mox_start))) {
        return;
    }

    /*
     * The third program is the fork of K4: a parent and a child share a page
     * until one of them writes, and the parent waits for the status of the
     * child. The shell runs it, so the machine has to keep both tasks apart
     * and alive at the same time.
     */
    if (!program_install(PROGRAM_FORK_PATH, _binary_fork_mox_start,
                         (k_size_t)(_binary_fork_mox_end - _binary_fork_mox_start))) {
        return;
    }

    /*
     * The fourth program is the edge of the system call: it hands write,
     * stat and read addresses that are not its memory, and the machine has
     * to answer EFAULT and stay alive. The addresses and the lines are in
     * programs/badptr.c; the smoke test reads them.
     */
    if (!program_install(PROGRAM_BADPTR_PATH, _binary_badptr_mox_start,
                         (k_size_t)(_binary_badptr_mox_end - _binary_badptr_mox_start))) {
        return;
    }
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
     * The program of the milestone runs before the shell: execve is a wait
     * now, so the init task is its parent and the shell does not exist until
     * the program has ended. That is the order of the transcript the smoke
     * test reads: the lines of /bin/hello, then the prompt.
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

    port_x86_64_init(multiboot_info);

    memset(&cfg, 0, sizeof(cfg));
    cfg.board = g_board.name;
    cfg.board_desc = g_board.desc;
    cfg.board_info = &g_board;

    kernel_boot(&cfg);

    kinfo("x86-64: the kernel is up, heap %u KB free of %u KB",
          (unsigned)(k_port_heap_free() / 1024), (unsigned)(k_port_heap_total() / 1024));

    /*
     * The filesystem, before the tasks: the volume is mounted and the images
     * of the programs are written to /bin. The program itself runs later, in
     * the init task: execve is a wait for a task now, and the boot context
     * has no scheduler under it to wait with.
     */
    filesystem_bring_up();

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
