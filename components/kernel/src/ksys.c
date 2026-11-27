#include "kernel/ksys.h"

#include <stdio.h>
#include <string.h>

#include "kernel/clock.h"
#include "kernel/config.h"
#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/exec.h"
#include "kernel/kfile.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/mm.h"
#include "kernel/port.h"
#include "kernel/port/exec.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/tty.h"
#include "kernel/vfs.h"
#include "kernel/wait.h"

/*
 * The table of the system calls. The index is the number of the call, so
 * the dispatcher is a single lookup. The numbers are the contract, they are
 * written down in kernel/ksys.h and they never change.
 */
static struct ksys_entry g_table[KSYS_COUNT];
static k_u32_t g_registered;

/* --- the checks at the edge --- */

/*
 * Copy bytes to and from the memory of the caller. On a machine with an
 * address space per program the port walks the page tables of the program
 * and answers whether the whole range is there; on the board, the simulator
 * and the null port the pointer is memory of the kernel already and the copy
 * is an ordinary one. The check is the reason the helper exists: a program
 * that hands the kernel an address of no page has to get EFAULT from the
 * call, not a page fault of ring 0 that takes the machine down with it.
 */
/*
 * Is the range memory the kernel may touch on behalf of the caller? A page of
 * a mapping of the program that is not there yet is not a mistake of the
 * caller: it has no page because nobody has touched it, and the copy below
 * would fault on it. The range is filled from the file or with zeros first
 * (kernel/mm.h) and then the tables of the port are asked, which is also what
 * says whether a page of the stack or of the text is there.
 */
static bool user_range(ksys_arg_t address, k_size_t size, bool write)
{
    k_mm_space_t space = k_mm_current();

    if (size == 0) {
        return true;
    }

    /*
     * The null page is never memory of a caller. A machine with page tables
     * says so in the walk below, but the host and the board have no window
     * to walk and would copy their own address 8 instead of answering
     * EFAULT - a crash of the process or of the machine for a bad pointer of
     * a program. The rule belongs here, where every port asks it.
     */
    if (address < K_MM_PAGE_SIZE || address > UINTPTR_MAX - size) {
        return false;
    }

    if (space != 0) {
        /* A range that runs out of the mappings is refused below, by the
         * walk of the tables; the pages before it are already made. */
        (void)k_mm_touch(space, address, size, write);
    }

    return k_port_exec_contains((const void *)(uintptr_t)address, size);
}

static int copy_from_user(void *dst, ksys_arg_t src, k_size_t size)
{
    if (size == 0) {
        return 0;
    }

    if (!user_range(src, size, false)) {
        return KERR_FAULT;
    }

    k_port_exec_user_access_begin();
    memcpy(dst, (const void *)(uintptr_t)src, size);
    k_port_exec_user_access_end();
    return 0;
}

static int copy_to_user(ksys_arg_t dst, const void *src, k_size_t size)
{
    if (size == 0) {
        return 0;
    }

    if (!user_range(dst, size, true)) {
        return KERR_FAULT;
    }

    k_port_exec_user_access_begin();
    memcpy((void *)(uintptr_t)dst, src, size);
    k_port_exec_user_access_end();
    return 0;
}

/*
 * A block of the kernel written to the memory of a program, for the calls
 * whose range was checked with k_port_exec_contains() and no k_mm_touch: the
 * same window as above, which is the one moment the kernel may touch a page
 * of ring 3 on a processor with SMAP (ADR 0052).
 */
static void block_to_user(ksys_arg_t dst, const void *src, k_size_t size)
{
    k_port_exec_user_access_begin();
    memcpy((void *)(uintptr_t)dst, src, size);
    k_port_exec_user_access_end();
}

/*
 * A path of a program is not trusted. The kernel must not walk past the end
 * of the memory of the caller looking for the terminator, so the search is
 * limited to the window the VFS keeps a path in: a path that does not end
 * inside it is refused before any filesystem sees it. Every filesystem of
 * the tree is mounted below the root, so a path that does not begin there
 * cannot name an object at all, and NULL or the empty string is not a path.
 *
 * Every byte is checked before it is read. On a machine with page tables a
 * byte that is not the memory of the program is EFAULT, which is an answer of
 * the call and not a fault that stops the machine; on the other ports the
 * check accepts the pointer as the kernel's own.
 */
static int path_check(const char *path)
{
    ksys_arg_t address = (ksys_arg_t)(uintptr_t)path;
    k_size_t i;

    if (path == NULL) {
        return KERR_INVAL;
    }

    /* A path may lie in a page of a mapping that nobody has touched: the
     * window of the name is made before it is walked, and the pages after the
     * terminator are not a question the call has to answer. */
    {
        k_mm_space_t space = k_mm_current();

        if (space != 0) {
            (void)k_mm_touch(space, address, KSYS_PATH_MAX + 1u, false);
        }
    }

    for (i = 0; i <= KSYS_PATH_MAX; i++) {
        char c;

        if (!k_port_exec_contains((const void *)(uintptr_t)(address + i), 1)) {
            return KERR_FAULT;
        }

        k_port_exec_user_access_begin();
        c = path[i];
        k_port_exec_user_access_end();

        if (i == 0) {
            if (c != '/') {
                return KERR_INVAL;
            }
        } else if (c == '\0') {
            return 0;
        }
    }

    return KERR_NAMETOOLONG;
}

/*
 * Copy a path of the caller into the kernel's own window. The VFS and every
 * filesystem under it then work with memory of the kernel, not with a pointer
 * the caller could still change; path_check has already found the terminator
 * inside the window, so the length is known and the whole string is memory of
 * the caller. The buffer of the caller is KSYS_PATH_MAX characters plus the
 * terminator, which is the buffer the VFS keeps.
 */
static int path_copy(char *out, ksys_arg_t path)
{
    const char *name = (const char *)(uintptr_t)path;
    k_size_t size;
    int rc = path_check(name);

    if (rc != 0) {
        return rc;
    }

    /*
     * The length is measured in the memory of the caller, so the read of the
     * string needs the same window as the copy below: `path_check` opens a
     * window for each byte it looks at, but it closes it before it returns,
     * and a processor with SMAP refuses this strlen in ring 0 (ADR 0052).
     * The terminator is already known to be inside the window, so a bad
     * pointer still ends in EFAULT from path_check and never reaches here.
     */
    k_port_exec_user_access_begin();
    size = strlen(name) + 1u;
    k_port_exec_user_access_end();

    return copy_from_user(out, path, size);
}

/*
 * The buffer of a call. NULL is only meaningful with a zero length, a length
 * above KSYS_MAX_IO cannot be a buffer of this system, and the range has to
 * be memory of the caller: each is refused before the kernel touches a byte.
 */
static int buffer_check(ksys_arg_t buf, ksys_arg_t count)
{
    k_mm_space_t space;

    if (count > KSYS_MAX_IO) {
        return KERR_TOOBIG;
    }

    if (buf == 0 && count != 0) {
        return KERR_INVAL;
    }

    if (count == 0) {
        return 0;
    }

    /*
     * A buffer of a program may be a page of a mapping that nobody has
     * touched yet: it is made before the copy, and the file behind it gives
     * the bytes. The write is tried first because a buffer of `read` is a
     * destination, and a page of the text - read-only - is made by the second
     * call and then read as the code it is.
     */
    space = k_mm_current();

    if (space != 0) {
        (void)k_mm_touch(space, buf, count, true);
        (void)k_mm_touch(space, buf, count, false);
    }

    if (!k_port_exec_contains((const void *)(uintptr_t)buf, count)) {
        return KERR_FAULT;
    }

    return 0;
}

/*
 * The flags of an open. The access is one of the three modes - the pair of
 * the bits is not a mode - and every other bit has to be a flag of the
 * contract. A program that asks for something the kernel does not know is
 * answered here, not by the filesystem under it.
 */
static int open_flags_check(ksys_arg_t flags)
{
    int access = (int)(flags & KSYS_O_ACCMODE);

    if ((flags & ~(ksys_arg_t)KSYS_O_ALLOWED) != 0) {
        return KERR_INVAL;
    }

    if (access != KSYS_O_RDONLY && access != KSYS_O_WRONLY && access != KSYS_O_RDWR) {
        return KERR_INVAL;
    }

    return 0;
}

/* --- the mode of an object --- */

/* The bits of one triad of a mode: read, write, execute. */
#define KSYS_MODE_READ 04u
#define KSYS_MODE_WRITE 02u
#define KSYS_MODE_EXEC 01u

/*
 * The uid of the task at the edge of the system call. A call before the
 * scheduler has a record names the kernel itself, whose uid is root.
 */
static k_u32_t caller_uid(void)
{
    struct task *self = task_self();

    return (self != NULL) ? task_get_uid(self) : KSYS_UID;
}

/*
 * The bits of a mode that decide for the caller. The rule lives in the VFS
 * (vfs_access_bits) and not here, because the check of the write of a
 * directory and the search of a path ask the same question: one answer for
 * the edge of the system and the tree above the filesystems, or the two doors
 * disagree about who may write what.
 */
static k_u32_t caller_bits(const struct vfs_stat *info)
{
    return vfs_access_bits(info);
}

/*
 * May the caller execute an object? The triad decides for a task of another
 * uid, and root runs a file that has an x bit for somebody: the bypass of
 * root covers the read and the write, not the mark of a program, exactly as
 * POSIX says it.
 */
#if KERNEL_EXEC
static bool exec_bits_allow(const struct vfs_stat *info)
{
    if (caller_uid() == KSYS_UID) {
        return (info->mode & 0111u) != 0u;
    }

    return (caller_bits(info) & KSYS_MODE_EXEC) != 0u;
}
#endif /* KERNEL_EXEC */

/*
 * The access of an open against the mode of an object. A filesystem that
 * keeps no modes answers with a zeroed one, and that zero is not "nobody may
 * touch it": it is a virtual filesystem (procfs, devfs) whose objects keep
 * the access they always had.
 */
static int open_mode_check(const struct vfs_stat *info, int access)
{
    k_u32_t bits;

    if (info->mode == 0u) {
        return 0;
    }

    bits = caller_bits(info);

    if (access != KSYS_O_WRONLY && (bits & KSYS_MODE_READ) == 0u) {
        return KERR_PERM;
    }

    if (access != KSYS_O_RDONLY && (bits & KSYS_MODE_WRITE) == 0u) {
        return KERR_PERM;
    }

    return 0;
}

/*
 * The mode of the file that is about to be opened. The object is described
 * by the filesystem and the mode is not guessed from the flags of the call:
 * the mode is the number the filesystem keeps, the flags are the wish of the
 * caller, and the two are compared here.
 *
 * A file that is not there is not checked: with O_CREAT it is about to be
 * made with the mode of the design, which allows both accesses, and without
 * O_CREAT the open itself answers ENOENT - the same answer the check would
 * have to give.
 */
static int open_path_mode_check(const char *path, int flags)
{
    struct vfs_stat info;
    int rc;

    rc = vfs_stat(path, &info);
    if (rc != 0) {
        return (rc == KERR_NOENT) ? 0 : rc;
    }

    return open_mode_check(&info, flags & KSYS_O_ACCMODE);
}

/* --- the open files of a task --- */

static struct kfile *fd_get(ksys_arg_t fd)
{
    struct task *self = task_self();

    /*
     * The descriptor is an index of the table of the task and it arrives as
     * a raw register: a value that does not fit the table is not a
     * descriptor, and the cast to int must not turn one into one.
     */
    if (fd >= (ksys_arg_t)TASK_MAX_FDS) {
        return NULL;
    }

    return kfile_get(self, (k_u32_t)fd);
}

/* --- the handlers --- */

static k_ssize_t sys_exit(ksys_arg_t code, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    /*
     * A program is not a task of the kernel on a machine that runs it in its
     * own address space: exiting it means coming back to the call that
     * entered it with the status, not ending the task the caller runs in.
     * The port knows which of the two is asking; a machine whose programs are
     * tasks (the board, the simulator) says false and takes the old path.
     */
    if (k_port_exec_active()) {
        k_port_exec_exit((int)code);
        return 0;
    }

    task_exit((int)code);
    return 0;
}

static k_ssize_t sys_read(ksys_arg_t fd, ksys_arg_t buf, ksys_arg_t count, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);
    int rc;

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        /*
         * The descriptor 0 is the terminal of the task when nothing was
         * opened on it, the same rule as the console of 1 and 2 for a write
         * (ADR 0051 §2): a program that was written for a Unix finds its
         * keyboard there and never opens `/dev/tty`.
         */
        if (fd == 0) {
            rc = buffer_check(buf, count);
            if (rc != 0) {
                return rc;
            }

            k_ssize_t got;

            /*
             * The window of SMAP (ADR 0052): the terminal of the console
             * writes the buffer of the program itself.
             */
            k_port_exec_user_access_begin();
            got = k_tty_read((char *)(uintptr_t)buf, (k_size_t)count);
            k_port_exec_user_access_end();
            return got;
        }

        return KERR_BADF;
    }

    /*
     * The descriptor remembers how it was opened: a file that was opened
     * to be written to cannot be read from (POSIX: EBADF). The rule lives
     * here, at the edge of the system, so that every filesystem answers
     * the same way.
     */
    if ((file->flags & KSYS_O_ACCMODE) == KSYS_O_WRONLY) {
        return KERR_BADF;
    }

    rc = buffer_check(buf, count);
    if (rc != 0) {
        return rc;
    }

    if (count == 0) {
        /* POSIX: a request of no bytes reads nothing and is not an error. */
        return 0;
    }

    {
        k_ssize_t got;

        /* The files of the VFS write the buffer of the program in ring 0, and
         * the window of ADR 0052 is the call the program asked for. */
        k_port_exec_user_access_begin();
        got = kfile_read(file, (void *)(uintptr_t)buf, count);
        k_port_exec_user_access_end();
        return got;
    }
}

static k_ssize_t sys_write(ksys_arg_t fd, ksys_arg_t buf, ksys_arg_t count, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);
    int rc;

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file != NULL) {
        /* The descriptor remembers how it was opened: a file that was
         * opened to be read from cannot be written to (POSIX: EBADF). */
        if ((file->flags & KSYS_O_ACCMODE) == KSYS_O_RDONLY) {
            return KERR_BADF;
        }

        rc = buffer_check(buf, count);
        if (rc != 0) {
            return rc;
        }

        if (count == 0) {
            return 0;
        }

        {
            k_ssize_t wrote;

            k_port_exec_user_access_begin();
            wrote = kfile_write(file, (const void *)(uintptr_t)buf, count);
            k_port_exec_user_access_end();
            return wrote;
        }
    }

    /*
     * The descriptors 1 and 2 are the console of the system when they are
     * not open as a file, the same way as in Linux.
     */
    if (fd == 1 || fd == 2) {
        rc = buffer_check(buf, count);
        if (rc != 0) {
            return rc;
        }

        k_port_exec_user_access_begin();
        k_console_write((const char *)(uintptr_t)buf, (k_size_t)count);
        k_port_exec_user_access_end();
        return (k_ssize_t)count;
    }

    return KERR_BADF;
}

/*
 * A copy of a name that a compiler cannot read as a truncation: the loop
 * stops one byte before the end and the last write is the zero. The kernel
 * has no string header of its own, and `strncpy` followed by a zero - the
 * idiom every libc offers - is still a truncation to GCC at -O2.
 */
static void copy_bounded(char *dst, k_size_t size, const char *src)
{
    k_size_t i = 0;

    if (size == 0) {
        return;
    }
    if (src != NULL) {
        while (i + 1 < size && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static k_ssize_t sys_open(ksys_arg_t path, ksys_arg_t flags, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    struct kfile *file = NULL;
    int fd;
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    rc = open_flags_check(flags);
    if (rc != 0) {
        return rc;
    }

    /*
     * A directory is read, never written: O_DIRECTORY goes with O_RDONLY
     * and with no other flag. The flag of the directory is part of the
     * flags of the open, so it is checked before a descriptor is spent.
     */
    if ((flags & KSYS_O_DIRECTORY) != 0 && flags != KSYS_O_DIRECTORY) {
        return KERR_INVAL;
    }

    /*
     * The mode of the file decides whether the access that was asked for is
     * allowed. The check is here, at the edge, and not in the filesystem, so
     * that every filesystem under it answers a forbidden open the same way.
     */
    rc = open_path_mode_check(name, (int)flags);
    if (rc != 0) {
        return rc;
    }

    fd = kfile_alloc(task_self(), &file);
    if (fd < 0) {
        return fd;
    }

    rc = vfs_open(name, (int)flags, &file->file, file->priv);
    if (rc != 0) {
        kfile_discard(task_self(), fd);
        return rc;
    }

    file->flags = (int)flags;
    copy_bounded(file->path, sizeof(file->path), name);

    return (k_ssize_t)fd;
}

static k_ssize_t sys_close(ksys_arg_t fd, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL || fd >= (ksys_arg_t)TASK_MAX_FDS) {
        return KERR_BADF;
    }

    return kfile_close(self, (int)fd);
}

static k_ssize_t sys_fsync(ksys_arg_t fd, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        return KERR_BADF;
    }

    /* A pipe is not a file of a volume: nothing of it can be brought to a
     * medium, and the answer is KERR_INVAL as in POSIX. */
    if (file->kind == KFILE_PIPE) {
        return KERR_INVAL;
    }

    return vfs_fsync(&file->file);
}

static k_ssize_t sys_pipe(ksys_arg_t fds, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    int ends[2];
    int rc;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    /*
     * The answer is two descriptors and the caller has to give the place
     * for them: NULL is a mistake, and a place outside the memory of the
     * caller is EFAULT before a pipe is spent on it.
     */
    if (fds == 0) {
        return KERR_INVAL;
    }

    if (!k_port_exec_contains((const void *)(uintptr_t)fds, sizeof(ends))) {
        return KERR_FAULT;
    }

    rc = kfile_pipe(self, ends);
    if (rc != 0) {
        return rc;
    }

    block_to_user(fds, ends, sizeof(ends));

    return 0;
}

static k_ssize_t sys_dup2(ksys_arg_t oldfd, ksys_arg_t newfd, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    /*
     * Both numbers are indexes of the table of the task. A register that
     * holds a wider number is not a descriptor, and POSIX answers EBADF for
     * a descriptor that is not open or a number that is not one.
     */
    if (oldfd >= (ksys_arg_t)TASK_MAX_FDS || newfd >= (ksys_arg_t)TASK_MAX_FDS) {
        return KERR_BADF;
    }

    return kfile_dup2(self, (int)oldfd, (int)newfd);
}

/*
 * The step of a `poll` that waits on something other than a pipe or the
 * console. Such a list has no channel to arm - the state of a regular file
 * and of a device of the VFS is always the one it reports - so the wait is a
 * plain sleep and the list is looked at again after each step: `poll` with no
 * descriptor and no deadline is a wait of the life of the machine, and a step
 * keeps it from being one number of ticks that wraps.
 */
#define KSYS_POLL_IDLE_MS 10u

/*
 * The readiness of one descriptor for `poll`. The answer is the bits of
 * revents, and the rules are the ones of the object: a pipe knows whether a
 * byte is there, a regular file of this system never makes a read wait, and
 * the console of the system has a key only when the port says so.
 *
 * A number that is not an open descriptor is POLLERR and not EBADF: the list
 * of a poll is one answer, and the error of one entry is a bit of that entry
 * (the contract of the three bits is in kernel/ksys.h).
 */
static k_u16_t poll_entry(ksys_arg_t fd, k_u16_t events)
{
    struct kfile *file;
    k_u16_t revents = 0;

    /* A negative descriptor is ignored, as POSIX has it. */
    if ((k_i32_t)fd < 0) {
        return 0;
    }

    file = fd_get(fd);

    if (file == NULL) {
        /* The descriptors 1 and 2 are the console of the system when they
         * are not open as a file (the same rule as write), and it is always
         * ready to take a byte. The descriptor 0 is the terminal of the task,
         * ready when a key waits (ADR 0051 §2). */
        if (fd == 1 || fd == 2) {
            return (k_u16_t)(events & (KSYS_POLLIN | KSYS_POLLOUT));
        }

        if (fd == 0) {
            return ((events & KSYS_POLLIN) != 0 && k_tty_input_ready()) ? KSYS_POLLIN : 0;
        }

        return KSYS_POLLERR;
    }

    /*
     * The access of the descriptor is the first answer: a file that was
     * opened to be written to cannot be read, and a poll that asks whether
     * such a read would wait asks about an operation the descriptor refuses.
     */
    if (((events & KSYS_POLLIN) != 0 && (file->flags & KSYS_O_ACCMODE) == KSYS_O_WRONLY) ||
        ((events & KSYS_POLLOUT) != 0 && (file->flags & KSYS_O_ACCMODE) == KSYS_O_RDONLY)) {
        return KSYS_POLLERR;
    }

    /*
     * The console is a device of the VFS whose read waits for a line: it is
     * ready when a character is waiting, and the port is the only one that
     * knows. The path of the open file is the name it was opened with, which
     * is the whole of the identity a device of this system has.
     */
    if (strcmp(file->path, "/dev/tty0") == 0 || strcmp(file->path, "/dev/tty") == 0) {
        if ((events & KSYS_POLLIN) != 0 && k_tty_input_ready()) {
            revents |= KSYS_POLLIN;
        }

        if ((events & KSYS_POLLOUT) != 0) {
            revents |= KSYS_POLLOUT;
        }

        return revents;
    }

    return kfile_poll(file, events);
}

static k_ssize_t sys_poll(ksys_arg_t fds, ksys_arg_t nfds, ksys_arg_t timeout, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    struct ksys_pollfd list[KSYS_POLL_MAX];
    k_u32_t count;
    k_u32_t i;
    k_u32_t wait_ms;
    bool infinite;
    k_u64_t deadline = 0;
    k_size_t size;
    int rc;

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (nfds > (ksys_arg_t)KSYS_POLL_MAX) {
        return KERR_INVAL;
    }

    /*
     * The timeout is an int: -1 is "without a deadline", a number that does
     * not fit an int is not a timeout, and a negative number that is not -1
     * is not one either. The width of the register is checked before the
     * cast, so a wide value cannot become a valid timeout by being cut.
     */
    if (timeout == (ksys_arg_t)-1) {
        infinite = true;
        wait_ms = K_WAIT_INFINITE;
    } else if (timeout > (ksys_arg_t)0x7FFFFFFFu) {
        return KERR_INVAL;
    } else {
        infinite = false;
        wait_ms = (k_u32_t)timeout;
    }

    size = (k_size_t)nfds * sizeof(list[0]);

    if (nfds > 0) {
        if (fds == 0) {
            return KERR_INVAL;
        }

        rc = copy_from_user(list, fds, size);
        if (rc != 0) {
            return rc;
        }
    }

    if (!infinite) {
        deadline = k_sched_uptime_ms() + (k_u64_t)wait_ms;
    }

    for (;;) {
        k_u16_t events;
        bool piped = false;
        bool console = false;
        bool armed = false;
        k_u32_t step;

        count = 0;

        /*
         * The channels this list can be woken by, from the kind of each
         * descriptor: the kind does not change under the call. The record is
         * armed before the list is looked at, so a state that changes while
         * this loop runs is either seen by the look below or wakes the
         * record - there is no window in which the wakeup finds nothing
         * (docs/debts.md, K15).
         */
        for (i = 0; i < (k_u32_t)nfds; i++) {
            struct kfile *file = ((k_i32_t)list[i].fd < 0) ? NULL : fd_get(list[i].fd);

            if (file != NULL) {
                if (file->kind == KFILE_PIPE) {
                    piped = true;
                } else if (strcmp(file->path, "/dev/tty0") == 0) {
                    console = true;
                }
            }
        }

        if (piped || console) {
            armed = k_wait_arm(piped ? K_WAIT_CHANNEL_PIPES : NULL,
                               console ? K_WAIT_CHANNEL_CONSOLE : NULL);
        }

        for (i = 0; i < (k_u32_t)nfds; i++) {
            events = (k_u16_t)(list[i].events & (KSYS_POLLIN | KSYS_POLLOUT));
            list[i].revents = poll_entry(list[i].fd, events);

            if (list[i].revents != 0) {
                count++;
            }
        }

        if (count > 0) {
            if (armed) {
                k_wait_disarm();
            }
            break;
        }

        if (!infinite) {
            k_u64_t now = k_sched_uptime_ms();

            if (now >= deadline) {
                if (armed) {
                    k_wait_disarm();
                }
                break;
            }

            wait_ms = (k_u32_t)(deadline - now);
        }

        /*
         * The wait is the scheduler's: the task leaves the run queue and a
         * pipe or the console wakes it when the state it watches changes -
         * at once, not at the next tick, because the record is armed and the
         * wake of the channel moves the task back into the run queue.
         */
        if (!armed) {
            /*
             * Nothing of the list can wake a task (no pipe, no console): the
             * state is looked at once more after a step of the plain sleep
             * and the call answers. `poll` with no descriptor and no deadline
             * is a wait of the life of the machine, and a step keeps it from
             * being one number of ticks that wraps.
             */
            step = wait_ms;

            if (step > KSYS_POLL_IDLE_MS) {
                step = KSYS_POLL_IDLE_MS;
            }

            if (!infinite) {
                k_u64_t now = k_sched_uptime_ms();

                if (now >= deadline) {
                    break;
                }

                step = (k_u32_t)K_MIN((k_u64_t)step, deadline - now);
            }

            k_sched_sleep_ms(step);
            continue;
        }

        rc = k_wait_sleep(wait_ms);
        k_wait_disarm();

        if (rc == K_WAIT_SIGNAL) {
            return KERR_INTR;
        }

        if (rc == K_WAIT_TIMEOUT && infinite) {
            continue;
        }

        if (rc == K_WAIT_TIMEOUT) {
            /* The deadline passed, or nothing of the list can wake a task:
             * the state is read once more and then the call answers. */
            count = 0;

            for (i = 0; i < (k_u32_t)nfds; i++) {
                events = (k_u16_t)(list[i].events & (KSYS_POLLIN | KSYS_POLLOUT));
                list[i].revents = poll_entry(list[i].fd, events);

                if (list[i].revents != 0) {
                    count++;
                }
            }

            break;
        }
    }

    if (nfds > 0) {
        rc = copy_to_user(fds, list, size);
        if (rc != 0) {
            return rc;
        }
    }

    return (k_ssize_t)count;
}

/* --- the memory of a program --- */

/*
 * The path behind a descriptor for a mapping. A mapping keeps its own open
 * file of the name, so that closing the descriptor of the program does not
 * take the file of the mapping away, and the name is what the VFS opened it
 * with. A pipe has no file and a directory is not a file of bytes.
 */
static int mmap_file(ksys_arg_t fd, char *path)
{
    struct kfile *file = fd_get(fd);

    if (file == NULL) {
        return KERR_BADF;
    }

    if (file->kind == KFILE_PIPE) {
        return KERR_SPIPE;
    }

    copy_bounded(path, VFS_PATH_MAX, file->path);

    return 0;
}

static k_ssize_t sys_mmap(ksys_arg_t addr, ksys_arg_t len, ksys_arg_t prot, ksys_arg_t flags,
                          ksys_arg_t fd, ksys_arg_t offset)
{
    char path[VFS_PATH_MAX];
    k_mm_space_t space = k_mm_current();
    k_u64_t placed = 0;
    k_u64_t window;
    bool anonymous;
    int rc;

    if (len == 0 || (len % KSYS_PAGE_SIZE) != 0 || (offset % KSYS_PAGE_SIZE) != 0) {
        return KERR_INVAL;
    }

    if ((prot & ~(ksys_arg_t)(KSYS_PROT_READ | KSYS_PROT_WRITE | KSYS_PROT_EXEC)) != 0 ||
        (flags & ~(ksys_arg_t)(KSYS_MAP_SHARED | KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS)) != 0) {
        return KERR_INVAL;
    }

    /* One of the two sharing modes, and not both: a mapping is either a copy
     * of the file or the file itself. */
    if (((flags & KSYS_MAP_SHARED) != 0) == ((flags & KSYS_MAP_PRIVATE) != 0)) {
        return KERR_INVAL;
    }

    if ((addr % KSYS_PAGE_SIZE) != 0) {
        return KERR_INVAL;
    }

    if (space == 0) {
        return KERR_NOSYS;
    }

    window = k_port_mm_top() - k_port_mm_base();
    if (len > window) {
        return KERR_NOMEM;
    }

    anonymous = (flags & KSYS_MAP_ANONYMOUS) != 0;
    path[0] = '\0';

    if (!anonymous) {
        rc = mmap_file(fd, path);
        if (rc != 0) {
            return rc;
        }
    }

    rc = k_mm_map(space, addr, (k_size_t)len, (k_u32_t)prot, (k_u32_t)flags,
                  anonymous ? NULL : path, offset, &placed);
    if (rc != 0) {
        return rc;
    }

    return (k_ssize_t)placed;
}

static k_ssize_t sys_munmap(ksys_arg_t addr, ksys_arg_t len, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    k_mm_space_t space = k_mm_current();

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if ((addr % KSYS_PAGE_SIZE) != 0 || len == 0 || (len % KSYS_PAGE_SIZE) != 0) {
        return KERR_INVAL;
    }

    if (space == 0) {
        return KERR_NOSYS;
    }

    return k_mm_unmap(space, addr, (k_size_t)len);
}

static k_ssize_t sys_msync(ksys_arg_t addr, ksys_arg_t len, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    k_mm_space_t space = k_mm_current();

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if ((addr % KSYS_PAGE_SIZE) != 0 || len == 0 || (len % KSYS_PAGE_SIZE) != 0) {
        return KERR_INVAL;
    }

    if (space == 0) {
        return KERR_NOSYS;
    }

    return k_mm_msync(space, addr, (k_size_t)len);
}

static k_ssize_t sys_brk(ksys_arg_t addr, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                         ksys_arg_t a4, ksys_arg_t a5)
{
    k_mm_space_t space = k_mm_current();
    struct task *self = task_self();
    k_u64_t before = 0;
    k_u64_t value = 0;
    int rc;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (space == 0) {
        return KERR_NOSYS;
    }

    /*
     * The heap is memory of the task as the blocks of `malloc` are, so it is
     * charged to the same budget (KSYS_RLIMIT_MEMORY) and a program that
     * allocates has a ceiling the shell can lower. The growth is checked
     * before the break moves: a refusal leaves the heap where it was.
     */
    rc = k_mm_brk(space, 0, &before);
    if (rc != 0) {
        return rc;
    }

    if (self != NULL && addr > before && !task_memory_fits(self, (k_size_t)(addr - before))) {
        return KERR_NOMEM;
    }

    rc = k_mm_brk(space, addr, &value);
    if (rc != 0) {
        return rc;
    }

    if (self != NULL) {
        if (value > before) {
            task_memory_charge(self, (k_size_t)(value - before));
        } else if (before > value) {
            task_memory_uncharge(self->pid, (k_size_t)(before - value));
        }
    }

    return (k_ssize_t)value;
}

static k_ssize_t sys_stat(ksys_arg_t path, ksys_arg_t buf, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    struct vfs_stat stat;
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    /*
     * A program that asks for the information has to give the place for
     * the answer: a NULL there is a mistake, not "the answer is not
     * wanted", and the filesystem must not be asked for nothing.
     */
    if (buf == 0) {
        return KERR_INVAL;
    }

    /*
     * The answer goes to the memory of the caller through the copy at the
     * edge: an address that is not part of the program is EFAULT and not a
     * write of ring 0 into a page that is not there.
     */
    if (!k_port_exec_contains((const void *)(uintptr_t)buf, sizeof(stat))) {
        return KERR_FAULT;
    }

    rc = vfs_stat(name, &stat);
    if (rc != 0) {
        return rc;
    }

    block_to_user(buf, &stat, sizeof(stat));

    return 0;
}

static k_ssize_t sys_statx(ksys_arg_t path, ksys_arg_t buf, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    struct vfs_statx stat;
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    /* The place for the answer is the caller's, as for stat. */
    if (buf == 0) {
        return KERR_INVAL;
    }

    if (!k_port_exec_contains((const void *)(uintptr_t)buf, sizeof(stat))) {
        return KERR_FAULT;
    }

    rc = vfs_statx(name, &stat);
    if (rc != 0) {
        return rc;
    }

    block_to_user(buf, &stat, sizeof(stat));

    return 0;
}

/*
 * The information about an open object. A descriptor of this system keeps the
 * name it was opened with, so the call is the statx of that name: the object
 * that a rename moved since the open answers for the name it had, which is
 * the one fact the descriptor still knows. A pipe has no name and no statx;
 * the library answers a pipe from its own knowledge of it (ESPIPE on a seek)
 * and never reaches this call.
 */
static k_ssize_t sys_fstat(ksys_arg_t fd, ksys_arg_t buf, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);
    struct vfs_statx stat;
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        return KERR_BADF;
    }

    if (file->kind == KFILE_PIPE) {
        return KERR_SPIPE;
    }

    if (buf == 0) {
        return KERR_INVAL;
    }

    if (!k_port_exec_contains((const void *)(uintptr_t)buf, sizeof(stat))) {
        return KERR_FAULT;
    }

    rc = vfs_statx(file->path, &stat);
    if (rc != 0) {
        return rc;
    }

    /*
     * The size of an open file is the one the descriptor holds: MicroFS
     * writes the record when the descriptor is closed or synced, so the
     * medium can still say the length of the open. The mode, the owner and
     * the moments are the ones of the record and do not wait for a close.
     */
    if (stat.type == 1) {
        stat.size = file->file.size;
    }

    block_to_user(buf, &stat, sizeof(stat));

    return 0;
}

static k_ssize_t sys_lseek(ksys_arg_t fd, ksys_arg_t offset, ksys_arg_t whence, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);
    k_ssize_t position;

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        return KERR_BADF;
    }

    /* A pipe has no position: there is nothing to seek in it. */
    if (file->kind == KFILE_PIPE) {
        return KERR_SPIPE;
    }

    /*
     * The whence is part of the signature of the call: three values, and
     * the number arrives as a raw register, so it is compared before the
     * cast to int can cut a wide number down to a valid one.
     */
    if (whence > KSYS_SEEK_END) {
        return KERR_INVAL;
    }

    switch ((int)whence) {
    case KSYS_SEEK_SET:
        position = (k_ssize_t)offset;
        break;
    case KSYS_SEEK_CUR:
        position = (k_ssize_t)file->file.offset + (k_ssize_t)offset;
        break;
    case KSYS_SEEK_END:
        position = (k_ssize_t)file->file.size + (k_ssize_t)offset;
        break;
    default:
        return KERR_INVAL;
    }

    if (position < 0) {
        return KERR_INVAL;
    }

    file->file.offset = (ksys_arg_t)position;

    return position;
}

static k_ssize_t sys_ftruncate(ksys_arg_t fd, ksys_arg_t size, ksys_arg_t a2, ksys_arg_t a3,
                               ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        return KERR_BADF;
    }

    /* A pipe is a ring of bytes and not a file with a size: the call has
     * nothing to cut, and POSIX answers EINVAL here. */
    if (file->kind == KFILE_PIPE) {
        return KERR_INVAL;
    }

    /* The edge checks the access, as it does for read and write: the
     * descriptor of a reader stays a descriptor, but it has no cut. */
    if ((file->flags & KSYS_O_ACCMODE) == KSYS_O_RDONLY) {
        return KERR_BADF;
    }

    return vfs_truncate(&file->file, (k_u64_t)size);
}

static k_ssize_t sys_unlink(ksys_arg_t path, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    int rc;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    return vfs_unlink(name);
}

static k_ssize_t sys_rmdir(ksys_arg_t path, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    int rc;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    return vfs_rmdir(name);
}

static k_ssize_t sys_mkdir(ksys_arg_t path, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    int rc;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    return vfs_mkdir(name);
}

/*
 * The rename of the ABI (ADR 0024 §5). Two paths of one filesystem; the
 * volume answers whether it can be atomic, and a caller of the installer
 * reads KERR_NOTSUP as "the window is not closed".
 */
static k_ssize_t sys_rename(ksys_arg_t from, ksys_arg_t to, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    char from_name[KSYS_PATH_MAX + 1];
    char to_name[KSYS_PATH_MAX + 1];
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(from_name, from);
    if (rc != 0) {
        return rc;
    }

    rc = path_copy(to_name, to);
    if (rc != 0) {
        return rc;
    }

    return vfs_rename(from_name, to_name);
}

static k_ssize_t sys_readdir(ksys_arg_t fd, ksys_arg_t buf, ksys_arg_t count, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);
    struct vfs_dirent *out = (struct vfs_dirent *)(uintptr_t)buf;
    k_size_t max;
    k_size_t count_files = 0;
    int rc;

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        return KERR_BADF;
    }

    /* A pipe has no entries: only a descriptor of a directory does. */
    if (file->kind == KFILE_PIPE) {
        return KERR_NOTDIR;
    }

    if (out == NULL) {
        return KERR_INVAL;
    }

    if (count > KSYS_MAX_IO) {
        return KERR_TOOBIG;
    }

    /*
     * The buffer is an array of the entries, not a number of bytes: a count
     * that is not a whole number of them would let the last entry land past
     * the end of the memory of the caller.
     */
    if ((count % sizeof(struct vfs_dirent)) != 0) {
        return KERR_INVAL;
    }

    /*
     * The array is written in place through the descriptor, so the whole
     * range is checked before the directory is asked of a byte of it: a
     * program that names the guard page or a page it never mapped gets
     * EFAULT, not a write of ring 0 into no page.
     */
    if (!k_port_exec_contains(out, count)) {
        return KERR_FAULT;
    }

    max = count / sizeof(struct vfs_dirent);
    if (max == 0) {
        return 0;
    }

    /*
     * The entries are read through the descriptor, and its position goes
     * with them: the next call continues where this one stopped. The
     * directory is the one that was opened with O_DIRECTORY; a descriptor
     * of a file answers ENOTDIR in the VFS.
     */
    k_port_exec_user_access_begin();
    rc = vfs_readdir(&file->file, out, max, &count_files);
    k_port_exec_user_access_end();

    if (rc != 0) {
        return rc;
    }

    return (k_ssize_t)(count_files * sizeof(struct vfs_dirent));
}

static k_ssize_t sys_getpid(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();

    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return (self != NULL) ? (k_ssize_t)self->pid : 0;
}

static k_ssize_t sys_getcwd(ksys_arg_t buf, ksys_arg_t size, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    const char *cwd;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    cwd = task_get_cwd(self);
    if (cwd == NULL) {
        return KERR_INVAL;
    }

    /* The program has to give the place for the path and its terminator:
     * a NULL or a zero size cannot hold even the root. */
    if (buf == 0 || size == 0) {
        return KERR_INVAL;
    }

    {
        k_size_t len = strlen(cwd) + 1;
        int rc;

        if (len > size) {
            return KERR_TOOBIG;
        }

        rc = copy_to_user(buf, cwd, len);
        if (rc != 0) {
            return rc;
        }
    }

    return (k_ssize_t)strlen(cwd);
}

/*
 * Move the working directory of the caller (number 48). The task keeps one
 * path, so the call is the whole of the answer: a program that moves the
 * directory moves it for the programs it starts, and for the child of a fork,
 * because the two are copies of the same record. A path that does not name a
 * directory is refused before the task is touched, and a name that does not
 * fit TASK_CWD_MAX is not cut to fit (the rule of `cd` in the shell).
 */
static k_ssize_t sys_chdir(ksys_arg_t path, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    struct vfs_stat info;
    struct task *self = task_self();
    int rc;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    rc = vfs_stat(name, &info);
    if (rc != 0) {
        return rc;
    }

    if (info.type != 2) {
        return KERR_NOTDIR;
    }

    if (!task_set_cwd(self, name)) {
        return KERR_NAMETOOLONG;
    }

    return 0;
}

/*
 * One of the two clocks in the shape of POSIX (number 49). CLOCK_REALTIME is
 * the wall clock of the machine with the fraction of the second that passed
 * since it was set, CLOCK_MONOTONIC the millisecond count of the boot, which
 * is what `uptime` answers. A clock that is not one of the two is EINVAL, and
 * the answer has to have a place: a caller that wants no answer does not call.
 */
static k_ssize_t sys_clock_gettime(ksys_arg_t clock_id, ksys_arg_t buf, ksys_arg_t a2,
                                   ksys_arg_t a3, ksys_arg_t a4, ksys_arg_t a5)
{
    struct ksys_timespec moment;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (buf == 0) {
        return KERR_INVAL;
    }

    if (clock_id == (ksys_arg_t)KSYS_CLOCK_REALTIME) {
        struct k_clock_moment now;

        k_clock_now(&now);
        moment.tv_sec = (k_i64_t)now.sec;
        moment.tv_nsec = (k_i64_t)now.nsec;
    } else if (clock_id == (ksys_arg_t)KSYS_CLOCK_MONOTONIC) {
        k_u64_t milliseconds = k_sched_uptime_ms();

        moment.tv_sec = (k_i64_t)(milliseconds / 1000u);
        moment.tv_nsec = (k_i64_t)((milliseconds % 1000u) * 1000000u);
    } else {
        return KERR_INVAL;
    }

    return copy_to_user(buf, &moment, sizeof(moment));
}

/*
 * The processor time of the caller (number 50). The scheduler already counts
 * it for the CPU limit of the task, in milliseconds, and `task_cpu_tick()`
 * brings the count up to the moment of the call before it is read, so a
 * program that asks twice sees the time that passed between the two.
 */
static k_ssize_t sys_getrusage(ksys_arg_t buf, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                               ksys_arg_t a4, ksys_arg_t a5)
{
    struct ksys_rusage usage;
    struct task *self = task_self();

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    if (buf == 0) {
        return KERR_INVAL;
    }

    (void)task_cpu_tick(self);

    usage.utime_ms = (k_u64_t)self->cpu_ms;
    usage.stime_ms = 0;

    return copy_to_user(buf, &usage, sizeof(usage));
}

static k_ssize_t sys_ioctl(ksys_arg_t fd, ksys_arg_t cmd, ksys_arg_t arg, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        /*
         * The descriptors 0, 1 and 2 are the terminal of the task while they
         * are not open: a program asks it for the mode and the size before it
         * draws anything (ADR 0051 §2, §3).
         */
        if (fd <= 2) {
            return k_tty_ioctl((k_u32_t)cmd, (k_u64_t)arg);
        }

        return KERR_BADF;
    }

    /*
     * The command goes to the filesystem of the open file, which is where its
     * meaning lives: a device of /dev answers the commands of its driver and
     * a file of a volume has none (ADR 0028). A request outside the width of
     * the field is not a command of this ABI.
     */
    if (cmd > 0xFFFFFFFFu) {
        return KERR_INVAL;
    }

    return (k_ssize_t)vfs_ioctl(&file->file, (k_u32_t)cmd, (k_u64_t)arg);
}

static k_ssize_t sys_yield(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    k_sched_yield();

    return 0;
}

static k_ssize_t sys_sleep(ksys_arg_t ms, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    k_sched_sleep_ms(ms);

    return (k_ssize_t)ms;
}

static k_ssize_t sys_uptime(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return (k_ssize_t)k_sched_uptime_ms();
}

static k_ssize_t sys_reboot(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    k_port_reset();

    return 0;
}

/*
 * The program of the child of a fork. The child starts here, marks itself
 * and then runs the program of the parent. The mark is set before the
 * program starts, so that the child can never take the way of the parent.
 */
static void forked_child(void *arg)
{
    struct task *self = task_self();
    struct task *parent = (struct task *)arg;

    if (self != NULL) {
        self->is_child = true;
    }

    if (parent != NULL && parent->entry != NULL) {
        parent->entry(parent->arg);
    }
}

/*
 * The fork: a task with the same program. On a machine with an address space
 * per task the port copies the context of the caller (`k_port_fork_copy`):
 * the child gets its own memory, its own page tables and returns from this
 * same call with zero, and the parent keeps the pid. A machine without an MMU
 * cannot copy the memory (KERNEL_NOMMU), so the port refuses and the child is
 * the placeholder of the kernel: a task that shares the memory of the parent
 * and re-enters its program from the start. The child of that placeholder
 * tells itself by the zero and by the mark of its task.
 */
static k_ssize_t sys_fork(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3, ksys_arg_t a4,
                          ksys_arg_t a5)
{
    struct task *self = task_self();
    struct task *child;
    char name[TASK_NAME_MAX];

    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    /*
     * A task that is itself the child of the placeholder and reaches the
     * fork again is that child: it gets the 0 of POSIX, and no second child
     * is made. The child of the placeholder re-runs the program of the parent
     * from its start, so a program that does not ask "am I the child?" first
     * would fork again and again without this. A child of a real fork does
     * not take this way: it continues after the call with the zero.
     */
    if (self->is_child) {
        return 0;
    }

    /* The name of the child is the name of the parent with a mark. */
    {
        k_size_t len = strlen(self->name);

        if (len + 2 > sizeof(name)) {
            len = sizeof(name) - 2;
        }
        memcpy(name, self->name, len);
        name[len] = '\'';
        name[len + 1] = '\0';
    }

    /*
     * The port copies the context of a program of ring 3. It answers -1 when
     * the machine cannot copy a context at all, which is the case the
     * placeholder below is for, and -2 when the copy ran out of memory.
     */
    {
        struct task *copied = NULL;
        int rc = task_fork(self, name, &copied);

        if (rc == 0) {
            return (k_ssize_t)copied->pid;
        }

        if (rc == -2) {
            return KERR_NOSPC;
        }
    }

    child = task_fork_shared(self, name, forked_child, (void *)self);
    if (child == NULL) {
        return KERR_NOSPC;
    }

    return (k_ssize_t)child->pid;
}

static k_ssize_t sys_getppid(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();

    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL || self->parent == NULL) {
        return 0;
    }

    return (k_ssize_t)self->parent->pid;
}

static k_ssize_t sys_getpgid(ksys_arg_t pid, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return (k_ssize_t)task_getpgid((k_u32_t)pid);
}

static k_ssize_t sys_setpgid(ksys_arg_t pid, ksys_arg_t pgid, ksys_arg_t a2, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return (k_ssize_t)task_setpgid((k_u32_t)pid, (k_u32_t)pgid);
}

/* --- the credentials of the caller --- */

/*
 * The real uid: the person. It is the identity `whoami` of a setuid program
 * names and the one that comes back when the program drops what it was given
 * (ADR 0053). The permission checks never read it.
 */
static k_ssize_t sys_getuid(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();

    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return (self != NULL) ? (k_ssize_t)task_get_ruid(self) : 0;
}

/*
 * The effective uid: the identity every check of the kernel reads. It is the
 * real uid for a task of the shell, and the owner of the file while a program
 * of the setuid flag of MOX1 runs (ADR 0053).
 */
static k_ssize_t sys_geteuid(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return (k_ssize_t)caller_uid();
}

static k_ssize_t sys_getgid(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();

    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return (self != NULL) ? (k_ssize_t)task_get_gid(self) : 0;
}

/*
 * Change the uid of the caller, with the three uids of POSIX (ADR 0053). *
 * Root - the effective uid zero - names any uid of the fifteen bits a record
 * holds, and the three uids become that number: this is how a program that
 * entered with the identity of its owner gives it up (`setuid(getuid())`,
 * which also clears the saved uid, so the identity cannot be taken back).
 *
 * A task of another effective uid names its real uid or its saved uid, and
 * the effective uid moves to it: this is how a setuid program takes the
 * identity it was given and how it can leave and return to it. Any other
 * number is KERR_PERM. The real uid never moves but by the hand of root.
 */
static k_ssize_t sys_setuid(ksys_arg_t uid, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    k_u32_t target = (k_u32_t)uid;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    if ((uid & ~(ksys_arg_t)KSYS_UID_MAX) != 0) {
        return KERR_INVAL;
    }

    if (task_is_root(self)) {
        task_set_uid(self, target);
        return 0;
    }

    if (target != task_get_ruid(self) && target != task_get_suid(self)) {
        return KERR_PERM;
    }

    task_set_euid(self, target);

    return 0;
}

/* The same rule for the group, with the one gid this system has: there is no
 * saved gid and no image asks for one, so a task of another gid may only name
 * the number it already has. */
static k_ssize_t sys_setgid(ksys_arg_t gid, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    k_u32_t target = (k_u32_t)gid;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    if ((gid & ~(ksys_arg_t)KSYS_UID_MAX) != 0) {
        return KERR_INVAL;
    }

    if (!task_is_root(self) && target != task_get_gid(self)) {
        return KERR_PERM;
    }

    task_set_gid(self, target);

    return 0;
}

/*
 * A signal to one task. The system has no process groups, so only a positive
 * pid is a target: the two meanings of POSIX for 0 and -1 ("the group" and
 * "everybody") are not guessed at. A pid with no live task is ESRCH.
 *
 * A task of another uid is not a target of a task that is not root: a
 * program cannot end the shell that started it or a neighbour it does not
 * own. Root signals anybody, and a task always signals itself.
 */
static k_ssize_t sys_kill(ksys_arg_t pid, ksys_arg_t sig, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    struct task *target;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    /* The raw register has to survive the round trip through int, as in
     * waitpid: 2^32 + 5 is not the task 5. */
    if ((ksys_arg_t)(int)pid != pid || (int)pid <= 0) {
        return KERR_INVAL;
    }

    if ((ksys_arg_t)(int)sig != sig) {
        return KERR_INVAL;
    }

    target = task_by_pid((k_u32_t)(int)pid);
    if (target != NULL && self != NULL && !task_is_root(self) &&
        task_get_uid(target) != task_get_uid(self)) {
        return KERR_PERM;
    }

    return task_signal_send((k_u32_t)(int)pid, (int)sig);
}

/*
 * Install the answer of a signal and return the previous one. The handler is
 * memory of the caller - a program of ring 3 has to hand an address of its
 * window, and the port tells whether the range is there - so a broken pointer
 * is EFAULT now and not a fault of the program when the signal arrives.
 *
 * The answer travels as the raw value of a register: 0 is the default action,
 * 1 is "ignore", anything else is the handler, as POSIX and kernel/signal.h
 * say.
 */
static k_ssize_t sys_signal(ksys_arg_t sig, ksys_arg_t handler, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    task_signal_fn fn;
    task_signal_fn previous;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    if ((ksys_arg_t)(int)sig != sig) {
        return KERR_INVAL;
    }

    fn = (task_signal_fn)(uintptr_t)handler;

    if (fn != K_SIG_DFL && fn != K_SIG_IGN &&
        !k_port_exec_contains((const void *)(uintptr_t)handler, 1)) {
        return KERR_FAULT;
    }

    previous = task_signal_set(self, (int)sig, fn);
    if (previous == K_SIG_ERR) {
        return KERR_INVAL;
    }

    return (k_ssize_t)(uintptr_t)previous;
}

/*
 * The restorer of a signal frame. The port of a machine with ring 3 puts the
 * address of a few instructions of user code on the stack of a program when
 * it enters a handler; the last of them is this call, and its answer is the
 * value the interrupted context had in the register of the result. It is not
 * a call a program makes by itself, and on a machine without ring 3 there is
 * no frame to return to.
 */
static k_ssize_t sys_sigreturn(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                               ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    k_ssize_t restored = 0;
    int rc;

    K_UNUSED(a0);
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    rc = k_port_task_signal_leave(self, &restored);
    if (rc < 0) {
        return rc;
    }

    /* The answer is the value the interrupted context had: it may be a
     * negative answer of the call the handler interrupted. */
    return restored;
}

/* --- the limits of a task --- */

/* One of the three resources of a limit, or EINVAL for anything else. A raw
 * register that holds a wide number is not a selector. */
static int limit_which(ksys_arg_t which)
{
    if (which < (ksys_arg_t)KSYS_RLIMIT_MEMORY || which > (ksys_arg_t)KSYS_RLIMIT_FDS) {
        return KERR_INVAL;
    }

    return 0;
}

/*
 * The current limit of the caller. The answer is one 64-bit word, so it is
 * copied into the memory of the caller through the edge; a selector the ABI
 * does not know and a place that is not there are refused before the task is
 * asked anything.
 */
static k_ssize_t sys_getlimit(ksys_arg_t which, ksys_arg_t value, ksys_arg_t a2, ksys_arg_t a3,
                              ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    k_u64_t limit = 0;
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    rc = limit_which(which);
    if (rc != 0) {
        return rc;
    }

    if (value == 0) {
        return KERR_INVAL;
    }

    if (!k_port_exec_contains((const void *)(uintptr_t)value, sizeof(limit))) {
        return KERR_FAULT;
    }

    rc = task_limit_get(self, (k_u32_t)which, &limit);
    if (rc != 0) {
        return rc;
    }

    return copy_to_user(value, &limit, sizeof(limit));
}

/*
 * Lower a limit of the caller. The limits are the task's own: there is no
 * call that changes the limit of another task, and no selector for a set of
 * tasks. A raise is EPERM (task_limit_set explains the policy).
 */
static k_ssize_t sys_setlimit(ksys_arg_t which, ksys_arg_t value, ksys_arg_t a2, ksys_arg_t a3,
                              ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    rc = limit_which(which);
    if (rc != 0) {
        return rc;
    }

    return task_limit_set(self, (k_u32_t)which, (k_u64_t)value);
}

/*
 * Wait for a child. The number 0 or -1 means "any child". The status of
 * the child is the code it finished with. A call with no child to wait for
 * answers ECHILD, whether it was ready to wait or not.
 */
static k_ssize_t sys_waitpid(ksys_arg_t pid, ksys_arg_t status, ksys_arg_t flags, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    struct task *self = task_self();
    struct task *child = NULL;
    k_u32_t child_pid = 0;
    int child_exit = 0;
    k_u32_t waited = 0;
    const k_u32_t limit = 5000; /* five seconds */

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    /*
     * The pid arrives as a raw register. A value that does not survive the
     * round trip through int is not a pid - the cast below must not turn
     * 2^32 + 5 into the child 5 - and only 0 and -1 mean "any child", so a
     * third negative number is a mistake of the caller.
     */
    if ((ksys_arg_t)(int)pid != pid) {
        return KERR_INVAL;
    }

    if ((int)pid < -1) {
        return KERR_INVAL;
    }

    /* The flags of the call are one bit: the rest is not of the contract. */
    if ((flags & ~(ksys_arg_t)KSYS_WNOHANG) != 0) {
        return KERR_INVAL;
    }

    /*
     * The place for the status is checked before the wait starts: a bad
     * pointer is EFAULT now, not after five seconds of waiting for a child
     * whose status cannot be delivered.
     */
    if (status != 0 && !k_port_exec_contains((const void *)(uintptr_t)status, sizeof(int))) {
        return KERR_FAULT;
    }

    /* Look for a child that has finished. */
    for (waited = 0; waited < limit; waited++) {
        bool has_child = false;
        k_size_t i;

        /*
         * The state of a task is changed by the scheduler from another
         * context, so the scan and the copy of what the caller needs happen
         * under one critical section: reading state and exit code after it
         * would be a race with the task that has just finished.
         */
        k_port_enter_critical();

        for (i = 0; i < TASK_MAX_TASKS; i++) {
            struct task *t = task_get_by_index_unlocked(i);

            if (t == NULL || t->parent != self) {
                continue;
            }

            /*
             * The pid arrives in the unsigned register of the call, where
             * -1 is larger than every pid: the comparison is on the signed
             * value, or "any child" (0 and -1) would name a task that cannot
             * exist and a wait for one's own children would answer ECHILD.
             */
            if ((int)pid > 0 && (int)t->pid != (int)pid) {
                continue;
            }

            has_child = true;

            if (t->state == TASK_STATE_ZOMBIE) {
                child = t;
                child_pid = t->pid;
                child_exit = t->exit_code;
                break;
            }
        }

        k_port_exit_critical();

        if (child != NULL) {
            break;
        }

        /*
         * There is no child that this call could wait for: not a running
         * one, not a finished one. That is ECHILD, and the answer does not
         * depend on the flags.
         */
        if (!has_child) {
            return KERR_CHILD;
        }

        /*
         * A signal that has to be answered cuts the wait short, and the
         * answer of the call is EINTR: the caller resumes after the call,
         * the handler runs before it, and the child is still there to be
         * waited for on the next try. A child that has finished is scanned
         * first, so the SIGCHLD of that very child does not turn the wait
         * into an error on the way to its status.
         */
        if (task_signal_interrupts(self)) {
            return KERR_INTR;
        }

        if ((flags & KSYS_WNOHANG) != 0) {
            /* The caller does not want to wait: there is nothing yet. */
            return 0;
        }

        k_sched_sleep_ms(1);
    }

    if (child == NULL) {
        return KERR_CHILD;
    }

    if (status != 0) {
        block_to_user(status, &child_exit, sizeof(child_exit));
    }

    task_reap(child_pid);

    return (k_ssize_t)child_pid;
}

/*
 * The heap of the kernel, as a call. It is not part of the ABI of a program:
 * a block of `kmm_alloc` is a pointer of ring 0, which the page tables of a
 * program map without the user bit, so a program can neither read nor write
 * what it asks for - and `free` of an address it guessed would hand a block
 * of the kernel back to the allocator while the kernel still holds it. A
 * caller with an address space is refused (ADR 0052); a task of the kernel -
 * the shell, the init of the boot - is the caller this call is for.
 */
static bool kernel_heap_refused(void)
{
    static bool said;

    if (k_mm_current() == 0) {
        return false;
    }

    if (!said) {
        said = true;
        kwarn("ksys: a program asked for the heap of the kernel; the call is refused (ADR 0052)");
    }

    return true;
}

static k_ssize_t sys_malloc(ksys_arg_t size, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    void *block;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (size == 0 || kernel_heap_refused()) {
        return 0;
    }

    block = kmm_alloc((k_size_t)size, kmm_current_priority());
    if (block == NULL) {
        return 0;
    }

    return (k_ssize_t)(uintptr_t)block;
}

static k_ssize_t sys_free(ksys_arg_t ptr, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (ptr == 0 || kernel_heap_refused()) {
        return 0;
    }

    kmm_free((void *)(uintptr_t)ptr);

    return 0;
}

static k_ssize_t sys_realloc(ksys_arg_t ptr, ksys_arg_t size, ksys_arg_t a2, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    void *old_block = (void *)(uintptr_t)ptr;
    void *block;
    k_size_t old_size;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (kernel_heap_refused()) {
        return 0;
    }

    if (size <= 0) {
        kmm_free(old_block);
        return 0;
    }

    old_size = kmm_block_size(old_block);

    block = kmm_alloc((k_size_t)size, kmm_current_priority());
    if (block == NULL) {
        return 0;
    }

    if (old_size > 0) {
        memcpy(block, old_block, (old_size < (k_size_t)size) ? old_size : (k_size_t)size);
    }

    kmm_free(old_block);

    return (k_ssize_t)(uintptr_t)block;
}

/* --- the mode, the owner and the programs --- */

static k_ssize_t sys_chmod(ksys_arg_t path, ksys_arg_t mode, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    /*
     * The mode is the nine bits of the triads: a wider number would have to
     * be cut to fit the record of the filesystem, and a cut number is not
     * what the caller asked for. A zero passes this door and is refused by
     * the filesystem, which is where the reason for that lives.
     */
    if ((mode & ~(ksys_arg_t)KSYS_MODE_MASK) != 0) {
        return KERR_INVAL;
    }

    return vfs_chmod(name, (k_u32_t)mode);
}

static k_ssize_t sys_chown(ksys_arg_t path, ksys_arg_t owner, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    /* The same for the fifteen bits of a uid. */
    if ((owner & ~(ksys_arg_t)KSYS_UID_MAX) != 0) {
        return KERR_INVAL;
    }

    return vfs_chown(name, (k_u32_t)owner);
}

static k_ssize_t sys_chgrp(ksys_arg_t path, ksys_arg_t group, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_check((const char *)(uintptr_t)path);
    if (rc != 0) {
        return rc;
    }

    /* The gid has the width of a uid; a wider number would spill into the
     * type of the record, so it is refused here and not cut. */
    if ((group & ~(ksys_arg_t)KSYS_GID_MAX) != 0) {
        return KERR_INVAL;
    }

    return vfs_chgrp((const char *)(uintptr_t)path, (k_u32_t)group);
}

/*
 * The program. The file is read through the VFS, its header is checked
 * against the format of the system, and the port is asked to run it: only
 * the port knows whether this machine can run a program at all.
 *
 * The image has to be in the memory of the kernel before the port copies it
 * to the window of the machine, so it is read into one block - the largest
 * transfer the edge of the system allows in one call, which is far more than
 * a program of this system needs. The block is given back on every way out
 * but the successful one: a port that enters a program does not return, and
 * the memory of the task goes with the program.
 *
 * The vector of the arguments is the memory of the caller. The kernel cannot
 * check an array of pointers without an MMU, but it does not have to walk it
 * at the moment the port enters the program: the strings are copied into the
 * frame of this call, which stays alive for as long as the program runs (a
 * program that returns comes back here, and one that never returns keeps the
 * stack of its caller). A program is therefore started with a vector the
 * kernel built, not with the pointers of a caller that could still change
 * them.
 */

/* The vector of a program, as a line of the shell can make one. */
#define KSYS_ARGV_MAX 8u
#define KSYS_ARGV_BYTES 512u

/*
 * Copy the vector at the address `from` and the strings it points at into
 * `args` and `words`, and terminate the vector. Returns 0, or E2BIG for a
 * vector or a string that does not fit, or EFAULT for a word of the vector or
 * a byte of a string that is not memory of the caller. A zero vector, or one
 * whose first word is zero, is an empty vector and not an error.
 *
 * The arguments and the environment of execve are two vectors of the same
 * shape, so they are copied by the same code into two frames: the bounds of
 * one never decide the room of the other.
 *
 * Every read goes through copy_from_user: the vector and the strings are the
 * memory of the caller, and on a machine with page tables an address that is
 * not mapped there is EFAULT instead of a fault of ring 0. The strings are
 * read one byte at a time and never past the room left, so a string that
 * never ends is stopped at the edge of the buffer instead of walking through
 * memory.
 */
#if KERNEL_EXEC
static int argv_copy(ksys_arg_t from, char *args[], char *words, k_size_t words_size)
{
    k_size_t used = 0;
    k_size_t count = 0;

    while (from != 0) {
        ksys_arg_t word = 0;
        k_size_t room;
        k_size_t length = 0;
        int rc;

        /* A vector whose end would wrap the address space has no words. */
        if (from > (ksys_arg_t)-1 - count * (ksys_arg_t)sizeof(ksys_arg_t)) {
            return KERR_FAULT;
        }

        rc = copy_from_user(&word, from + count * (ksys_arg_t)sizeof(ksys_arg_t), sizeof(word));
        if (rc != 0) {
            return rc;
        }

        if (word == 0) {
            break;
        }

        if (count >= KSYS_ARGV_MAX) {
            return KERR_TOOBIG;
        }

        room = words_size - used;
        while (length < room) {
            char byte;

            /* The end of the address space is not the end of a string. */
            if (word > (ksys_arg_t)-1 - length) {
                return KERR_FAULT;
            }

            rc = copy_from_user(&byte, word + length, 1);
            if (rc != 0) {
                return rc;
            }

            if (byte == '\0') {
                break;
            }

            words[used + length] = byte;
            length++;
        }

        if (length == room) {
            return KERR_TOOBIG;
        }

        words[used + length] = '\0';
        args[count] = words + used;
        used += length + 1;
        count++;
    }

    args[count] = NULL;

    return 0;
}
#endif /* KERNEL_EXEC */

#if KERNEL_EXEC
/* The credentials of the caller before a setuid run, to give them back. */
struct exec_creds {
    k_u32_t euid;
    k_u32_t suid;
};

/*
 * The setuid flag of an image (ADR 0053), the act of `execve` and not of the
 * parser: the file is about to become the program of this task, and the
 * identity of the program is the owner of that file.
 *
 * A file the caller may write is not a door: a setuid image that its caller
 * can replace would let the caller write the code that runs with the identity
 * of the owner, which is the classic hole of a setuid program and the reason
 * Linux clears the bit when the file is written. Root is the exception, as it
 * is for every triad: it may already write anything.
 *
 * The answer is 1 when the identity was applied, 0 when the image asks for
 * none, or KERR_PERM when the file may not carry the flag for this caller.
 * The saved uids are given back by exec_creds_end() when the program returns:
 * a task of the kernel that starts a program (the shell) waits for it and
 * must not keep the identity of the program it started. The one answer that
 * is not a return is K_EXEC_RUN_REPLACED, where the port has made the image
 * the memory and the entry of the very task that called the call - a program
 * that becomes another program, as POSIX has it. That task is the image from
 * then on, and it ends with it: nothing else of it runs with the identity, so
 * there is nothing to give back.
 */
static int exec_creds_begin(const struct vfs_stat *info, const struct k_exec_image *image,
                            struct exec_creds *saved)
{
    struct task *self = task_self();

    if (!image->setuid || self == NULL) {
        return 0;
    }

    if (task_get_uid(self) != KSYS_UID && (vfs_access_bits(info) & KSYS_MODE_WRITE) != 0) {
        kwarn("exec: %s asks for the identity of its owner and the caller may write the file",
              (image->path != NULL) ? image->path : "the image");
        return KERR_PERM;
    }

    saved->euid = task_get_uid(self);
    saved->suid = task_get_suid(self);
    task_set_euid(self, info->owner);

    return 1;
}

/* The caller gets its own identity back, whatever the program did with it. */
static void exec_creds_end(const struct exec_creds *saved)
{
    struct task *self = task_self();

    if (self == NULL) {
        return;
    }

    task_set_uids(self, task_get_ruid(self), saved->euid, saved->suid);
}

static k_ssize_t sys_execve(ksys_arg_t path, ksys_arg_t argv, ksys_arg_t envp, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    char name[KSYS_PATH_MAX + 1];
    /* The place for the data of the filesystem of an open file. */
    k_u8_t file_data[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    struct vfs_stat info;
    struct k_exec_image image;
    /* The vectors the program starts with: the frame keeps them alive. */
    char *args[KSYS_ARGV_MAX + 1];
    char words[KSYS_ARGV_BYTES];
    char *env_args[KSYS_ARGV_MAX + 1];
    char env_words[KSYS_ARGV_BYTES];
    char *const *vector = NULL;
    char *const *environment = NULL;
    struct exec_creds creds;
    struct task *self = task_self();
    k_u8_t *data;
    k_size_t got = 0;
    int applied;
    int rc;

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_copy(name, path);
    if (rc != 0) {
        return rc;
    }

    /*
     * The vectors are copied before the file is read: a refused one ends
     * the call while nothing has been spent, and the copies are what the
     * port is going to hand the program. What the kernel keeps from here on
     * is the vectors it built, not the pointers of the caller.
     *
     * The environment is copied into a frame of its own and not appended to
     * the words of the arguments: the two are separate vectors in the C
     * entry, and the strings of one are never the strings of the other.
     */
    if (argv != 0) {
        rc = argv_copy(argv, args, words, sizeof(words));
        if (rc != 0) {
            return rc;
        }

        vector = args;
    }

    if (envp != 0) {
        rc = argv_copy(envp, env_args, env_words, sizeof(env_words));
        if (rc != 0) {
            return rc;
        }

        environment = env_args;
    }

    rc = vfs_stat(name, &info);
    if (rc != 0) {
        return rc;
    }

    /*
     * Only a file can be a program, and only an executable one is run. A
     * device of devfs is not a directory and not a program either: there is
     * nothing to copy and nothing to enter.
     */
    if (info.type == 2) {
        return KERR_ISDIR;
    }

    if (info.type != 1) {
        return KERR_PERM;
    }

    if (info.mode != 0u && !exec_bits_allow(&info)) {
        return KERR_PERM;
    }

    /*
     * A file outside the two limits of the format is not read at all, so a
     * wrong file of the volume cannot ask for the memory of the system: the
     * header is the smallest image, and K_EXEC_MAX_IMAGE is the largest a
     * call reads (8 MiB, docs/debts.md). The transfer edge of one call,
     * KSYS_MAX_IO, is not this limit: the loop below reads a larger image in
     * pieces of it.
     */
    rc = k_exec_image_check(info.size);
    if (rc != 0) {
        return (k_ssize_t)rc;
    }

    /*
     * A machine with page tables does not read the image: the header says
     * what the program is and the pages come from the file on the fault of
     * the program (kernel/mm.h). The copy of a program of 4 MiB - the buffer
     * and the pages - was the second copy the plan of 1.6 removes, and this
     * is the call where it was spent.
     *
     * The file is opened through the VFS and not through a descriptor: an
     * exec spends no descriptor of the caller, and a failed one leaves the
     * table of the task exactly as it was.
     */
    if (k_port_mm_supported()) {
        k_u8_t header[K_EXEC_HEADER_SIZE];

        rc = vfs_open(name, VFS_O_RDONLY, &file, file_data);
        if (rc != 0) {
            return rc;
        }

        got = 0;
        while (got < sizeof(header)) {
            int count = vfs_read(&file, header + got, sizeof(header) - got);

            if (count <= 0) {
                vfs_close(&file);
                return KERR_IO;
            }

            got += (k_size_t)count;
        }

        vfs_close(&file);

        if (k_exec_parse_header(header, (k_size_t)info.size, &image) != 0) {
            return KERR_INVAL;
        }

        /* The file of the image is the one the port maps: the name is the
         * frame of this call, which outlives the run of the program only on a
         * machine whose programs are tasks of the kernel, and there the port
         * copies what it needs before it returns. */
        /* The parser stored the header it was given as the bytes of the
         * image; the port reads the file, not that buffer. */
        image.data = NULL;
        image.path = name;
        image.envp = (const char *const *)environment;

        applied = exec_creds_begin(&info, &image, &creds);
        if (applied < 0) {
            return applied;
        }

        rc = k_exec_run(&image, vector);

        if (applied > 0 && rc != K_EXEC_RUN_REPLACED) {
            exec_creds_end(&creds);
        }

        if (rc == K_EXEC_RUN_REPLACED) {
            return 0;
        }

        if (rc >= 0) {
            return 0;
        }

        if (rc == K_EXEC_RUN_OTHER_ARCH) {
            return KERR_NOEXEC;
        }

        if (rc != K_EXEC_RUN_NO_MACHINE) {
            kwarn("exec: the port refused the image (rc=%d)", rc);
            return KERR_NOMEM;
        }

        return KERR_NOSYS;
    }

    /*
     * A machine without page tables reads the image into one block of the
     * kernel: it lives for the time of the copy, not for the life of the
     * program, and it is asked with the priority of a user buffer, so that a
     * run of programs does not fragment the internal RAM the kernel and the
     * drivers keep.
     */
    data = kmm_alloc((k_size_t)info.size, KMM_PRIORITY_USER);
    if (data == NULL) {
        kwarn("exec: no memory for %s (%u bytes): heap %u, psram %u", name, (unsigned)info.size,
              (unsigned)k_port_heap_free(), (unsigned)k_port_psram_free());
        return KERR_NOMEM;
    }

    rc = vfs_open(name, VFS_O_RDONLY, &file, file_data);
    if (rc != 0) {
        kmm_free(data);
        return rc;
    }

    while (got < (k_size_t)info.size) {
        int count = vfs_read(&file, data + got, (k_size_t)info.size - got);

        if (count <= 0) {
            vfs_close(&file);
            kmm_free(data);
            return KERR_IO;
        }

        got += (k_size_t)count;
    }

    vfs_close(&file);

    if (k_exec_parse(data, got, &image) != 0) {
        kmm_free(data);
        return KERR_INVAL;
    }

    /*
     * The environment travels with the image: it is not of the file, so the
     * parser left it empty and the call is the one that knows it. A NULL
     * vector here is "the environment of the system", which k_exec_run fills
     * in before the port sees the image. The cast only adds const: the call
     * owns the strings until the program ends and never writes them.
     */
    image.envp = (const char *const *)environment;

    applied = exec_creds_begin(&info, &image, &creds);
    if (applied < 0) {
        kmm_free(data);
        return applied;
    }

    rc = k_exec_run(&image, vector);

    if (applied > 0 && rc != K_EXEC_RUN_REPLACED) {
        exec_creds_end(&creds);
    }

    kmm_free(data);

    /*
     * The answers below are the contract of k_exec_run: a number from 0 to
     * 255 is a program that ran and returned with that status. The call itself
     * ends with 0, because execve's ABI answer is not a waitpid; the code is
     * recorded in the calling kernel task for the shell's pipeline status.
     * K_EXEC_RUN_NO_MACHINE is a machine that cannot run a program at all (the
     * simulator, the board until it can) and is ENOSYS; K_EXEC_RUN_OTHER_ARCH is
     * an image of another machine and is ENOEXEC before a byte of it runs, and
     * REFUSED is the image the port could not place in its window or could not
     * copy.
     *
     * K_EXEC_RUN_REPLACED is the POSIX shape: the caller was a program of
     * ring 3 and the port has already made the new image its memory and the
     * frame of this call the entry of the program. There is no status to
     * return - the program is the caller from now on, and its status will be
     * the answer of the waitpid of its parent - so the call answers 0 and the
     * iretq of the stub does the rest.
     */
    if (rc == K_EXEC_RUN_REPLACED) {
        return 0;
    }

    if (rc >= 0) {
        if (self != NULL) {
            self->exec_status = rc;
        }
        return 0;
    }

    if (rc == K_EXEC_RUN_OTHER_ARCH) {
        return KERR_NOEXEC;
    }

    if (rc != K_EXEC_RUN_NO_MACHINE) {
        kwarn("exec: the port refused the image (rc=%d)", rc);
        return KERR_NOMEM;
    }

    return KERR_NOSYS;
}
#else
/*
 * The image was built without the parser and the runtime (ADR 0010). The
 * number of the call stays: a program compiled for the full kernel asks it
 * and gets the answer of a machine that cannot run a program, ENOSYS.
 */
static k_ssize_t sys_execve(ksys_arg_t path, ksys_arg_t argv, ksys_arg_t envp, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(path);
    K_UNUSED(argv);
    K_UNUSED(envp);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return KERR_NOSYS;
}
#endif /* KERNEL_EXEC */

/* --- the table --- */

int ksys_register(k_u32_t number, const char *name, ksys_handler_fn handler)
{
    if (number >= KSYS_COUNT || handler == NULL) {
        return KERR_INVAL;
    }

    if (g_table[number].handler == NULL) {
        g_registered++;
    }

    g_table[number].number = number;
    g_table[number].name = name;
    g_table[number].handler = handler;

    return 0;
}

k_ssize_t ksys_call(k_u32_t number, ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                    ksys_arg_t a4, ksys_arg_t a5)
{
    k_ssize_t rc;

    if (number >= KSYS_COUNT || g_table[number].handler == NULL) {
        return KERR_NOSYS;
    }

    /*
     * The edge of a system call is a point where the task is known to be
     * running: the processor time since the last point is charged here, so
     * the CPU limit is accounted on every machine, including the host whose
     * timer cannot name the task it interrupted.
     */
    task_cpu_tick(task_self());

    rc = g_table[number].handler(a0, a1, a2, a3, a4, a5);

    /*
     * The way out of the kernel is the point where a task answers its
     * signals: the call has its answer and it is the one the interrupted
     * program will see, and a handler runs before the program gets it. A
     * task of the kernel with a handler of its own runs the handler here;
     * a program of ring 3 gets a frame on its stack from the port.
     */
    task_signal_check(task_self(), rc, true);

    return rc;
}

const char *ksys_name(k_u32_t number)
{
    if (number >= KSYS_COUNT) {
        return NULL;
    }

    return g_table[number].name;
}

k_u32_t ksys_count(void)
{
    return g_registered;
}

void ksys_init(void)
{
    memset(g_table, 0, sizeof(g_table));
    g_registered = 0;

    /*
     * The open files are forgotten with the table: a boot and a test that
     * resets the calls start from an empty system, not from the
     * descriptors of the run before.
     */
    kfile_init();

    ksys_register(KSYS_EXIT, "exit", sys_exit);
    ksys_register(KSYS_READ, "read", sys_read);
    ksys_register(KSYS_WRITE, "write", sys_write);
    ksys_register(KSYS_OPEN, "open", sys_open);
    ksys_register(KSYS_CLOSE, "close", sys_close);
    ksys_register(KSYS_STAT, "stat", sys_stat);
    ksys_register(KSYS_LSEEK, "lseek", sys_lseek);
    ksys_register(KSYS_UNLINK, "unlink", sys_unlink);
    ksys_register(KSYS_MKDIR, "mkdir", sys_mkdir);
    ksys_register(KSYS_READDIR, "readdir", sys_readdir);
    ksys_register(KSYS_GETPID, "getpid", sys_getpid);
    ksys_register(KSYS_GETCWD, "getcwd", sys_getcwd);
    ksys_register(KSYS_IOCTL, "ioctl", sys_ioctl);
    ksys_register(KSYS_YIELD, "yield", sys_yield);
    ksys_register(KSYS_SLEEP, "sleep", sys_sleep);
    ksys_register(KSYS_UPTIME, "uptime", sys_uptime);
    ksys_register(KSYS_REBOOT, "reboot", sys_reboot);
    ksys_register(KSYS_FORK, "fork", sys_fork);
    ksys_register(KSYS_GETPPID, "getppid", sys_getppid);
    ksys_register(KSYS_WAITPID, "waitpid", sys_waitpid);
    ksys_register(KSYS_MALLOC, "malloc", sys_malloc);
    ksys_register(KSYS_FREE, "free", sys_free);
    ksys_register(KSYS_REALLOC, "realloc", sys_realloc);
    ksys_register(KSYS_CHMOD, "chmod", sys_chmod);
    ksys_register(KSYS_CHOWN, "chown", sys_chown);
    ksys_register(KSYS_CHGRP, "chgrp", sys_chgrp);
    ksys_register(KSYS_EXECVE, "execve", sys_execve);
    ksys_register(KSYS_STATX, "statx", sys_statx);
    ksys_register(KSYS_PIPE, "pipe", sys_pipe);
    ksys_register(KSYS_DUP2, "dup2", sys_dup2);
    ksys_register(KSYS_KILL, "kill", sys_kill);
    ksys_register(KSYS_SIGNAL, "signal", sys_signal);
    ksys_register(KSYS_SIGRETURN, "sigreturn", sys_sigreturn);
    ksys_register(KSYS_GETLIMIT, "getlimit", sys_getlimit);
    ksys_register(KSYS_SETLIMIT, "setlimit", sys_setlimit);
    ksys_register(KSYS_RENAME, "rename", sys_rename);
    ksys_register(KSYS_FTRUNCATE, "ftruncate", sys_ftruncate);
    ksys_register(KSYS_GETUID, "getuid", sys_getuid);
    ksys_register(KSYS_GETEUID, "geteuid", sys_geteuid);
    ksys_register(KSYS_GETGID, "getgid", sys_getgid);
    ksys_register(KSYS_SETUID, "setuid", sys_setuid);
    ksys_register(KSYS_SETGID, "setgid", sys_setgid);
    ksys_register(KSYS_RMDIR, "rmdir", sys_rmdir);
    ksys_register(KSYS_FSTAT, "fstat", sys_fstat);
    ksys_register(KSYS_POLL, "poll", sys_poll);
    ksys_register(KSYS_MMAP, "mmap", sys_mmap);
    ksys_register(KSYS_MUNMAP, "munmap", sys_munmap);
    ksys_register(KSYS_MSYNC, "msync", sys_msync);
    ksys_register(KSYS_BRK, "brk", sys_brk);
    ksys_register(KSYS_CHDIR, "chdir", sys_chdir);
    ksys_register(KSYS_CLOCK_GETTIME, "clock_gettime", sys_clock_gettime);
    ksys_register(KSYS_GETRUSAGE, "getrusage", sys_getrusage);
    ksys_register(KSYS_GETPGID, "getpgid", sys_getpgid);
    ksys_register(KSYS_SETPGID, "setpgid", sys_setpgid);
    ksys_register(KSYS_FSYNC, "fsync", sys_fsync);
}

/* --- the wrappers --- */

k_ssize_t ksys_read(int fd, void *buf, k_size_t count)
{
    return ksys_call(KSYS_READ, (ksys_arg_t)fd, (ksys_arg_t)(uintptr_t)buf, (ksys_arg_t)count, 0, 0,
                     0);
}

k_ssize_t ksys_write(int fd, const void *buf, k_size_t count)
{
    return ksys_call(KSYS_WRITE, (ksys_arg_t)fd, (ksys_arg_t)(uintptr_t)buf, (ksys_arg_t)count, 0,
                     0, 0);
}

int ksys_open(const char *path, int flags)
{
    return (int)ksys_call(KSYS_OPEN, (ksys_arg_t)(uintptr_t)path, (ksys_arg_t)flags, 0, 0, 0, 0);
}

int ksys_close(int fd)
{
    return (int)ksys_call(KSYS_CLOSE, (ksys_arg_t)fd, 0, 0, 0, 0, 0);
}

int ksys_fsync(int fd)
{
    return (int)ksys_call(KSYS_FSYNC, (ksys_arg_t)fd, 0, 0, 0, 0, 0);
}

int ksys_stat(const char *path, void *buf)
{
    return (int)ksys_call(KSYS_STAT, (ksys_arg_t)(uintptr_t)path, (ksys_arg_t)(uintptr_t)buf, 0, 0,
                          0, 0);
}

int ksys_statx(const char *path, void *buf)
{
    return (int)ksys_call(KSYS_STATX, (ksys_arg_t)(uintptr_t)path, (ksys_arg_t)(uintptr_t)buf, 0, 0,
                          0, 0);
}

k_ssize_t ksys_lseek(int fd, k_ssize_t offset, int whence)
{
    return ksys_call(KSYS_LSEEK, (ksys_arg_t)fd, (ksys_arg_t)offset, (ksys_arg_t)whence, 0, 0, 0);
}

int ksys_unlink(const char *path)
{
    return (int)ksys_call(KSYS_UNLINK, (ksys_arg_t)(uintptr_t)path, 0, 0, 0, 0, 0);
}

int ksys_mkdir(const char *path)
{
    return (int)ksys_call(KSYS_MKDIR, (ksys_arg_t)(uintptr_t)path, 0, 0, 0, 0, 0);
}

int ksys_rmdir(const char *path)
{
    return (int)ksys_call(KSYS_RMDIR, (ksys_arg_t)(uintptr_t)path, 0, 0, 0, 0, 0);
}

int ksys_fstat(int fd, void *buf)
{
    return (int)ksys_call(KSYS_FSTAT, (ksys_arg_t)fd, (ksys_arg_t)(uintptr_t)buf, 0, 0, 0, 0);
}

int ksys_rename(const char *from, const char *to)
{
    return (int)ksys_call(KSYS_RENAME, (ksys_arg_t)(uintptr_t)from, (ksys_arg_t)(uintptr_t)to, 0, 0,
                          0, 0);
}

int ksys_ftruncate(int fd, k_u64_t size)
{
    /* The arguments of a call are the width of a register: on the board a
     * size above 32 bits would arrive as its low half and cut the file to a
     * number nobody asked for. */
    if ((k_u64_t)(ksys_arg_t)size != size) {
        return KERR_TOOBIG;
    }

    return (int)ksys_call(KSYS_FTRUNCATE, (ksys_arg_t)fd, (ksys_arg_t)size, 0, 0, 0, 0);
}

k_ssize_t ksys_readdir(int fd, void *buf, k_size_t count)
{
    return ksys_call(KSYS_READDIR, (ksys_arg_t)fd, (ksys_arg_t)(uintptr_t)buf, (ksys_arg_t)count, 0,
                     0, 0);
}

k_u32_t ksys_getpid(void)
{
    return (k_u32_t)ksys_call(KSYS_GETPID, 0, 0, 0, 0, 0, 0);
}

k_u32_t ksys_getuid(void)
{
    return (k_u32_t)ksys_call(KSYS_GETUID, 0, 0, 0, 0, 0, 0);
}

k_u32_t ksys_geteuid(void)
{
    return (k_u32_t)ksys_call(KSYS_GETEUID, 0, 0, 0, 0, 0, 0);
}

k_u32_t ksys_getgid(void)
{
    return (k_u32_t)ksys_call(KSYS_GETGID, 0, 0, 0, 0, 0, 0);
}

int ksys_setuid(k_u32_t uid)
{
    return (int)ksys_call(KSYS_SETUID, (ksys_arg_t)uid, 0, 0, 0, 0, 0);
}

int ksys_setgid(k_u32_t gid)
{
    return (int)ksys_call(KSYS_SETGID, (ksys_arg_t)gid, 0, 0, 0, 0, 0);
}

int ksys_getcwd(char *buf, k_size_t size)
{
    return (int)ksys_call(KSYS_GETCWD, (ksys_arg_t)(uintptr_t)buf, (ksys_arg_t)size, 0, 0, 0, 0);
}

int ksys_yield(void)
{
    return (int)ksys_call(KSYS_YIELD, 0, 0, 0, 0, 0, 0);
}

int ksys_sleep(ksys_arg_t ms)
{
    return (int)ksys_call(KSYS_SLEEP, ms, 0, 0, 0, 0, 0);
}

k_u64_t ksys_uptime(void)
{
    return (ksys_arg_t)ksys_call(KSYS_UPTIME, 0, 0, 0, 0, 0, 0);
}

int ksys_reboot(void)
{
    return (int)ksys_call(KSYS_REBOOT, 0, 0, 0, 0, 0, 0);
}

int ksys_fork(void)
{
    return (int)ksys_call(KSYS_FORK, 0, 0, 0, 0, 0, 0);
}

int ksys_getppid(void)
{
    return (int)ksys_call(KSYS_GETPPID, 0, 0, 0, 0, 0, 0);
}

k_i32_t ksys_getpgid(k_u32_t pid)
{
    return (k_i32_t)ksys_call(KSYS_GETPGID, (ksys_arg_t)pid, 0, 0, 0, 0, 0);
}

int ksys_setpgid(k_u32_t pid, k_u32_t pgid)
{
    return (int)ksys_call(KSYS_SETPGID, (ksys_arg_t)pid, (ksys_arg_t)pgid, 0, 0, 0, 0);
}

int ksys_waitpid(int pid, int *status, int flags)
{
    return (int)ksys_call(KSYS_WAITPID, (ksys_arg_t)pid, (ksys_arg_t)(uintptr_t)status,
                          (ksys_arg_t)flags, 0, 0, 0);
}

void *ksys_malloc(k_size_t size)
{
    return (void *)(uintptr_t)ksys_call(KSYS_MALLOC, (ksys_arg_t)size, 0, 0, 0, 0, 0);
}

void ksys_free(void *ptr)
{
    ksys_call(KSYS_FREE, (ksys_arg_t)(uintptr_t)ptr, 0, 0, 0, 0, 0);
}

void *ksys_realloc(void *ptr, k_size_t size)
{
    return (void *)(uintptr_t)ksys_call(KSYS_REALLOC, (ksys_arg_t)(uintptr_t)ptr, (ksys_arg_t)size,
                                        0, 0, 0, 0);
}

int ksys_chmod(const char *path, k_u32_t mode)
{
    return (int)ksys_call(KSYS_CHMOD, (ksys_arg_t)(uintptr_t)path, (ksys_arg_t)mode, 0, 0, 0, 0);
}

int ksys_chown(const char *path, k_u32_t owner)
{
    return (int)ksys_call(KSYS_CHOWN, (ksys_arg_t)(uintptr_t)path, (ksys_arg_t)owner, 0, 0, 0, 0);
}

int ksys_chgrp(const char *path, k_u32_t group)
{
    return (int)ksys_call(KSYS_CHGRP, (ksys_arg_t)(uintptr_t)path, (ksys_arg_t)group, 0, 0, 0, 0);
}

int ksys_execve(const char *path, char *const argv[])
{
    return ksys_execve_env(path, argv, NULL);
}

int ksys_execve_env(const char *path, char *const argv[], char *const envp[])
{
    return (int)ksys_call(KSYS_EXECVE, (ksys_arg_t)(uintptr_t)path, (ksys_arg_t)(uintptr_t)argv,
                          (ksys_arg_t)(uintptr_t)envp, 0, 0, 0);
}

int ksys_pipe(int fds[2])
{
    return (int)ksys_call(KSYS_PIPE, (ksys_arg_t)(uintptr_t)fds, 0, 0, 0, 0, 0);
}

int ksys_dup2(int oldfd, int newfd)
{
    return (int)ksys_call(KSYS_DUP2, (ksys_arg_t)oldfd, (ksys_arg_t)newfd, 0, 0, 0, 0);
}

k_ssize_t ksys_mmap(void *addr, k_size_t len, k_u32_t prot, k_u32_t flags, int fd, k_u64_t offset)
{
    return ksys_call(KSYS_MMAP, (ksys_arg_t)(uintptr_t)addr, (ksys_arg_t)len, (ksys_arg_t)prot,
                     (ksys_arg_t)flags, (ksys_arg_t)(k_u32_t)fd, (ksys_arg_t)offset);
}

int ksys_munmap(void *addr, k_size_t len)
{
    return (int)ksys_call(KSYS_MUNMAP, (ksys_arg_t)(uintptr_t)addr, (ksys_arg_t)len, 0, 0, 0, 0);
}

int ksys_msync(void *addr, k_size_t len)
{
    return (int)ksys_call(KSYS_MSYNC, (ksys_arg_t)(uintptr_t)addr, (ksys_arg_t)len, 0, 0, 0, 0);
}

k_ssize_t ksys_brk(void *addr)
{
    return ksys_call(KSYS_BRK, (ksys_arg_t)(uintptr_t)addr, 0, 0, 0, 0, 0);
}

int ksys_chdir(const char *path)
{
    return (int)ksys_call(KSYS_CHDIR, (ksys_arg_t)(uintptr_t)path, 0, 0, 0, 0, 0);
}

int ksys_clock_gettime(k_u32_t clock, struct ksys_timespec *out)
{
    return (int)ksys_call(KSYS_CLOCK_GETTIME, (ksys_arg_t)clock, (ksys_arg_t)(uintptr_t)out, 0, 0,
                          0, 0);
}

int ksys_getrusage(struct ksys_rusage *out)
{
    return (int)ksys_call(KSYS_GETRUSAGE, (ksys_arg_t)(uintptr_t)out, 0, 0, 0, 0, 0);
}

int ksys_poll(struct ksys_pollfd *fds, k_u32_t nfds, int timeout_ms)
{
    return (int)ksys_call(KSYS_POLL, (ksys_arg_t)(uintptr_t)fds, (ksys_arg_t)nfds,
                          (ksys_arg_t)(k_u64_t)(k_i64_t)timeout_ms, 0, 0, 0);
}

int ksys_kill(int pid, int sig)
{
    return (int)ksys_call(KSYS_KILL, (ksys_arg_t)pid, (ksys_arg_t)sig, 0, 0, 0, 0);
}

task_signal_fn ksys_signal(int sig, task_signal_fn handler)
{
    k_ssize_t rc =
        ksys_call(KSYS_SIGNAL, (ksys_arg_t)sig, (ksys_arg_t)(uintptr_t)handler, 0, 0, 0, 0);

    if (rc < 0) {
        return K_SIG_ERR;
    }

    return (task_signal_fn)(uintptr_t)rc;
}

int ksys_getlimit(k_u32_t which, k_u64_t *value)
{
    return (int)ksys_call(KSYS_GETLIMIT, (ksys_arg_t)which, (ksys_arg_t)(uintptr_t)value, 0, 0, 0,
                          0);
}

int ksys_setlimit(k_u32_t which, k_u64_t value)
{
    return (int)ksys_call(KSYS_SETLIMIT, (ksys_arg_t)which, (ksys_arg_t)value, 0, 0, 0, 0);
}
