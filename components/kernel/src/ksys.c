#include "kernel/ksys.h"

#include <stdio.h>
#include <string.h>

#include "kernel/config.h"
#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/exec.h"
#include "kernel/kfile.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port/exec.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/vfs.h"

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
static int copy_from_user(void *dst, ksys_arg_t src, k_size_t size)
{
    if (size == 0) {
        return 0;
    }

    if (!k_port_exec_contains((const void *)(uintptr_t)src, size)) {
        return KERR_FAULT;
    }

    memcpy(dst, (const void *)(uintptr_t)src, size);
    return 0;
}

static int copy_to_user(ksys_arg_t dst, const void *src, k_size_t size)
{
    if (size == 0) {
        return 0;
    }

    if (!k_port_exec_contains((const void *)(uintptr_t)dst, size)) {
        return KERR_FAULT;
    }

    memcpy((void *)(uintptr_t)dst, src, size);
    return 0;
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

    for (i = 0; i <= KSYS_PATH_MAX; i++) {
        if (!k_port_exec_contains((const void *)(uintptr_t)(address + i), 1)) {
            return KERR_FAULT;
        }

        if (i == 0) {
            if (path[0] != '/') {
                return KERR_INVAL;
            }
        } else if (path[i] == '\0') {
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
    int rc = path_check(name);

    if (rc != 0) {
        return rc;
    }

    return copy_from_user(out, path, strlen(name) + 1);
}

/*
 * The buffer of a call. NULL is only meaningful with a zero length, a length
 * above KSYS_MAX_IO cannot be a buffer of this system, and the range has to
 * be memory of the caller: each is refused before the kernel touches a byte.
 */
static int buffer_check(ksys_arg_t buf, ksys_arg_t count)
{
    if (count > KSYS_MAX_IO) {
        return KERR_TOOBIG;
    }

    if (buf == 0 && count != 0) {
        return KERR_INVAL;
    }

    if (count != 0 && !k_port_exec_contains((const void *)(uintptr_t)buf, count)) {
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
 * The bits of a mode that decide for the caller. Root passes every triad; a
 * task whose uid is the owner of the object reads the triad of the owner and
 * every other task reads the triad of the others, because no filesystem of
 * this system stores a group (docs/debts.md).
 */
static k_u32_t caller_bits(const struct vfs_stat *info)
{
    k_u32_t uid = caller_uid();

    if (uid == KSYS_UID) {
        return 7u;
    }

    return (info->owner == uid) ? ((info->mode >> 6) & 7u) : (info->mode & 7u);
}

/*
 * May the caller execute an object? The triad decides for a task of another
 * uid, and root runs a file that has an x bit for somebody: the bypass of
 * root covers the read and the write, not the mark of a program, exactly as
 * POSIX says it.
 */
static bool exec_bits_allow(const struct vfs_stat *info)
{
    if (caller_uid() == KSYS_UID) {
        return (info->mode & 0111u) != 0u;
    }

    return (caller_bits(info) & KSYS_MODE_EXEC) != 0u;
}

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

    return kfile_read(file, (void *)(uintptr_t)buf, count);
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

        return kfile_write(file, (const void *)(uintptr_t)buf, count);
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

        k_console_write((const char *)(uintptr_t)buf, (k_size_t)count);
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

    memcpy((void *)(uintptr_t)fds, ends, sizeof(ends));

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

    memcpy((void *)(uintptr_t)buf, &stat, sizeof(stat));

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

    memcpy((void *)(uintptr_t)buf, &stat, sizeof(stat));

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
    int rc;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_check((const char *)(uintptr_t)path);
    if (rc != 0) {
        return rc;
    }

    return vfs_unlink((const char *)(uintptr_t)path);
}

static k_ssize_t sys_mkdir(ksys_arg_t path, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    int rc;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_check((const char *)(uintptr_t)path);
    if (rc != 0) {
        return rc;
    }

    return vfs_mkdir((const char *)(uintptr_t)path);
}

/*
 * The rename of the ABI (ADR 0024 §5). Two paths of one filesystem; the
 * volume answers whether it can be atomic, and a caller of the installer
 * reads KERR_NOTSUP as "the window is not closed".
 */
static k_ssize_t sys_rename(ksys_arg_t from, ksys_arg_t to, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_check((const char *)(uintptr_t)from);
    if (rc != 0) {
        return rc;
    }

    rc = path_check((const char *)(uintptr_t)to);
    if (rc != 0) {
        return rc;
    }

    return vfs_rename((const char *)(uintptr_t)from, (const char *)(uintptr_t)to);
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
    rc = vfs_readdir(&file->file, out, max, &count_files);
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

static k_ssize_t sys_ioctl(ksys_arg_t fd, ksys_arg_t cmd, ksys_arg_t arg, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
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

/* --- the credentials of the caller --- */

static k_ssize_t sys_getuid(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
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

/*
 * The effective uid. There is one uid in this kernel: a program cannot change
 * its identity without changing the identity its children inherit, because
 * the image of a program has no setuid bit and the kernel has no saved uid
 * to return to (docs/debts.md). The call exists because a portable source
 * asks for it, and it answers the number getuid answers.
 */
static k_ssize_t sys_geteuid(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    return sys_getuid(a0, a1, a2, a3, a4, a5);
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
 * Change the uid of the caller. Root may become any uid of the fifteen bits
 * a record of a filesystem holds; a task of another uid may only name the
 * number it already has, which is a no-op. A task that gives up root cannot
 * take it back with this call, which is the price of one credential and the
 * one way back is a reboot (or a login process, the other half of this work).
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

    if (!task_is_root(self) && target != task_get_uid(self)) {
        return KERR_PERM;
    }

    task_set_uid(self, target);

    return 0;
}

/* The same rule for the group; the gid decides no permission yet. */
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
        *(int *)(uintptr_t)status = child_exit;
    }

    task_reap(child_pid);

    return (k_ssize_t)child_pid;
}

/*
 * The memory of a program. The priority of the task decides where the
 * memory comes from, the same way as for the kernel itself.
 */
static k_ssize_t sys_malloc(ksys_arg_t size, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    void *block;

    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (size == 0) {
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

    if (ptr == 0) {
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
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_check((const char *)(uintptr_t)path);
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

    return vfs_chmod((const char *)(uintptr_t)path, (k_u32_t)mode);
}

static k_ssize_t sys_chown(ksys_arg_t path, ksys_arg_t owner, ksys_arg_t a2, ksys_arg_t a3,
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

    /* The same for the fifteen bits of a uid. */
    if ((owner & ~(ksys_arg_t)KSYS_UID_MAX) != 0) {
        return KERR_INVAL;
    }

    return vfs_chown((const char *)(uintptr_t)path, (k_u32_t)owner);
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
    k_u8_t *data;
    k_size_t got = 0;
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
     * The image is the largest block a call of this system asks for (up to
     * 64 KiB), and it lives for the time of the copy, not for the life of
     * the program. It is asked with the priority of a user buffer: on the
     * board that is the PSRAM, so a run of programs does not fragment the
     * internal RAM the kernel and the drivers keep, and a machine without
     * the PSRAM has one pool and behaves as before.
     */
    data = kmm_alloc((k_size_t)info.size, KMM_PRIORITY_USER);
    if (data == NULL) {
        kwarn("exec: no memory for %s (%u bytes): heap %u, psram %u", name, (unsigned)info.size,
              (unsigned)k_port_heap_free(), (unsigned)k_port_psram_free());
        return KERR_NOMEM;
    }

    /*
     * The file is opened through the VFS and not through a descriptor: an
     * exec spends no descriptor of the caller, and a failed one leaves the
     * table of the task exactly as it was.
     */
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

    rc = k_exec_run(&image, vector);
    kmm_free(data);

    /*
     * The answers below are the contract of k_exec_run: a number from 0 to
     * 255 is a program that ran and returned with that status - the call ends
     * with 0, because the status belongs to a process and the program is a
     * task now - K_EXEC_RUN_NO_MACHINE is a machine that cannot run a program
     * at all (the simulator, the board until it can) and is ENOSYS,
     * K_EXEC_RUN_OTHER_ARCH is an image of another machine and is ENOEXEC
     * before a byte of it runs, and REFUSED is the image the port could not
     * place in its window or could not copy.
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
