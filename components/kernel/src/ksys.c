#include "kernel/ksys.h"

#include <stdio.h>
#include <string.h>

#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/exec.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/port.h"
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
 * A path of a program is not trusted. The kernel must not walk past the end
 * of the memory of the caller looking for the terminator, so the search is
 * limited to the window the VFS keeps a path in: a path that does not end
 * inside it is refused before any filesystem sees it. Every filesystem of
 * the tree is mounted below the root, so a path that does not begin there
 * cannot name an object at all, and NULL or the empty string is not a path.
 */
static int path_check(const char *path)
{
    k_size_t i;

    if (path == NULL || path[0] != '/') {
        return KERR_INVAL;
    }

    for (i = 1; i <= KSYS_PATH_MAX; i++) {
        if (path[i] == '\0') {
            return 0;
        }
    }

    return KERR_TOOBIG;
}

/*
 * The buffer of a call. NULL is only meaningful with a zero length, and a
 * length above KSYS_MAX_IO cannot be a buffer of this system: the call is
 * refused before the kernel touches a byte of it.
 */
static int buffer_check(ksys_arg_t buf, ksys_arg_t count)
{
    if (count > KSYS_MAX_IO) {
        return KERR_TOOBIG;
    }

    if (buf == 0 && count != 0) {
        return KERR_INVAL;
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
 * The bits of a mode that decide for the caller. There is one user, uid
 * zero, and no group of its own: an object of that owner is answered by the
 * triad of the owner and an object of another owner by the triad of the
 * others - the caller is not that owner. There is no bypass for uid zero, so
 * a file of the mode 0200 cannot be read by the only user of the system
 * either: the rule of POSIX with the exception of root taken away (ADR 0005).
 */
static k_u32_t caller_bits(const struct vfs_stat *info)
{
    return (info->owner == KSYS_UID) ? ((info->mode >> 6) & 7u) : (info->mode & 7u);
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

    if (self == NULL || !self->files[fd].used) {
        return NULL;
    }

    return &self->files[fd];
}

/* Find a free slot and return its number, -1 when there is none. */
static int fd_alloc(void)
{
    struct task *self = task_self();
    int i;

    if (self == NULL) {
        return -1;
    }

    for (i = 0; i < TASK_MAX_FDS; i++) {
        if (!self->files[i].used) {
            memset(&self->files[i], 0, sizeof(self->files[i]));
            self->files[i].used = true;
            return i;
        }
    }

    return -1;
}

static void fd_release(int fd)
{
    struct task *self = task_self();

    if (self == NULL || fd < 0 || fd >= TASK_MAX_FDS) {
        return;
    }

    memset(&self->files[fd], 0, sizeof(self->files[fd]));
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

    return vfs_read(&file->file, (void *)(uintptr_t)buf, count);
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

        return vfs_write(&file->file, (const void *)(uintptr_t)buf, count);
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

static k_ssize_t sys_open(ksys_arg_t path, ksys_arg_t flags, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file;
    const char *name = (const char *)(uintptr_t)path;
    int fd;
    int rc;

    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_check(name);
    if (rc != 0) {
        return rc;
    }

    rc = open_flags_check(flags);
    if (rc != 0) {
        return rc;
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

    fd = fd_alloc();
    if (fd < 0) {
        return KERR_NOSPC;
    }

    file = &((struct task *)task_self())->files[fd];

    rc = vfs_open(name, (int)flags, &file->file, file->priv);
    if (rc != 0) {
        fd_release(fd);
        return rc;
    }

    file->flags = (int)flags;
    strncpy(file->path, name, sizeof(file->path) - 1);

    return (k_ssize_t)fd;
}

static k_ssize_t sys_close(ksys_arg_t fd, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
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

    vfs_close(&file->file);
    fd_release((int)fd);

    return 0;
}

static k_ssize_t sys_stat(ksys_arg_t path, ksys_arg_t buf, ksys_arg_t a2, ksys_arg_t a3,
                          ksys_arg_t a4, ksys_arg_t a5)
{
    struct vfs_stat stat;
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
     * A program that asks for the information has to give the place for
     * the answer: a NULL there is a mistake, not "the answer is not
     * wanted", and the filesystem must not be asked for nothing.
     */
    if (buf == 0) {
        return KERR_INVAL;
    }

    rc = vfs_stat((const char *)(uintptr_t)path, &stat);
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

    max = count / sizeof(struct vfs_dirent);
    if (max == 0) {
        return 0;
    }

    rc = vfs_list(file->path, out, max, &count_files);
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

        if (len > size) {
            return KERR_TOOBIG;
        }
        memcpy((void *)(uintptr_t)buf, cwd, len);
    }

    return (k_ssize_t)strlen(cwd);
}

static k_ssize_t sys_ioctl(ksys_arg_t fd, ksys_arg_t cmd, ksys_arg_t arg, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get(fd);

    K_UNUSED(cmd);
    K_UNUSED(arg);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        return KERR_BADF;
    }

    /* The files of the kernel have no control commands yet. */
    return KERR_INVAL;
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
 * The fork: a new task with the same program. The memory of the parent is
 * NOT copied - KERNEL_NOMMU is set, and a kernel without an MMU cannot do
 * that - so the tasks share it, like the flag CLONE_VM of Linux. The child
 * has its own number, so the program tells the child by the mark of its
 * task.
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
     * A task that is itself the child of a fork and reaches the fork again
     * is that child: it gets the 0 of POSIX, and no second child is made.
     * The child of this fork re-runs the program of the parent from its
     * start, so a program that does not ask "am I the child?" first would
     * fork again and again without this.
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

    child = task_create(name, forked_child, (void *)self, self->prio);
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

/*
 * Wait for a child. The number 0 or -1 means "any child". The status of
 * the child is the code it finished with.
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

    /* Look for a child that has finished. */
    for (waited = 0; waited < limit; waited++) {
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

            if (pid > 0 && (int)t->pid != (int)pid) {
                continue;
            }

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

        if ((flags & KSYS_WNOHANG) != 0) {
            /* The caller does not want to wait: there is nothing yet. */
            return 0;
        }

        k_sched_sleep_ms(1);
    }

    if (child == NULL) {
        return KERR_AGAIN;
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
        return KERR_NOSPC;
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
        return KERR_NOSPC;
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
 * The vector of the arguments is the memory of the caller. The kernel does
 * not walk it - without an MMU there is nothing to check an array of
 * pointers against - and it reaches the port as it is; today the port is
 * entered without one, because k_exec_run() has no vector in its signature.
 */
static k_ssize_t sys_execve(ksys_arg_t path, ksys_arg_t argv, ksys_arg_t a2, ksys_arg_t a3,
                            ksys_arg_t a4, ksys_arg_t a5)
{
    const char *name = (const char *)(uintptr_t)path;
    /* The place for the data of the filesystem of an open file. */
    k_u8_t file_data[VFS_FILE_DATA_SIZE];
    struct vfs_file file;
    struct vfs_stat info;
    struct k_exec_image image;
    k_u8_t *data;
    k_size_t got = 0;
    int rc;

    K_UNUSED(argv);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    rc = path_check(name);
    if (rc != 0) {
        return rc;
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

    if (info.mode != 0u && (caller_bits(&info) & KSYS_MODE_EXEC) == 0u) {
        return KERR_PERM;
    }

    /*
     * The smallest image is its header and the largest is the one the edge
     * passes in a single transfer; a file outside the two is not read at all,
     * so a wrong file of the volume cannot ask for the memory of the system.
     */
    if (info.size < K_EXEC_HEADER_SIZE || info.size > KSYS_MAX_IO) {
        return KERR_INVAL;
    }

    data = kmm_alloc((k_size_t)info.size, kmm_current_priority());
    if (data == NULL) {
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

    rc = k_exec_run(&image);
    kmm_free(data);

    /*
     * A port that never returns from the program never comes back here. The
     * three answers below are the contract of k_exec_run: 0 is a program
     * that ran and returned - the call ends, because a machine without a
     * process to exit into has to notice a program that finished - -2 is a
     * machine that cannot run a program at all (the board, the simulator),
     * and anything else is the image the port could not place in its window
     * or could not copy.
     */
    if (rc == 0) {
        return 0;
    }

    return (rc == -2) ? KERR_NOSYS : KERR_NOMEM;
}

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
    if (number >= KSYS_COUNT || g_table[number].handler == NULL) {
        return KERR_NOSYS;
    }

    return g_table[number].handler(a0, a1, a2, a3, a4, a5);
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

k_ssize_t ksys_readdir(int fd, void *buf, k_size_t count)
{
    return ksys_call(KSYS_READDIR, (ksys_arg_t)fd, (ksys_arg_t)(uintptr_t)buf, (ksys_arg_t)count, 0,
                     0, 0);
}

k_u32_t ksys_getpid(void)
{
    return (k_u32_t)ksys_call(KSYS_GETPID, 0, 0, 0, 0, 0, 0);
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
    return (int)ksys_call(KSYS_EXECVE, (ksys_arg_t)(uintptr_t)path, (ksys_arg_t)(uintptr_t)argv, 0,
                          0, 0, 0);
}
