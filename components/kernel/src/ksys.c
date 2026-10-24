#include "kernel/ksys.h"

#include <stdio.h>
#include <string.h>

#include "kernel/console.h"
#include "kernel/errno.h"
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

/* --- the open files of a task --- */

static struct kfile *fd_get(int fd)
{
    struct task *self = task_self();

    if (self == NULL || fd < 0 || fd >= TASK_MAX_FDS) {
        return NULL;
    }

    if (!self->files[fd].used) {
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
    struct kfile *file = fd_get((int)fd);

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

    return vfs_read(&file->file, (void *)(uintptr_t)buf, count);
}

static k_ssize_t sys_write(ksys_arg_t fd, ksys_arg_t buf, ksys_arg_t count, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get((int)fd);

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        /*
         * The descriptors 1 and 2 are the console of the system when they
         * are not open as a file, the same way as in Linux.
         */
        if (fd == 1 || fd == 2) {
            k_console_write((const char *)(uintptr_t)buf, (k_size_t)count);
            return (k_ssize_t)count;
        }
        return KERR_BADF;
    }

    /* The descriptor remembers how it was opened: a file that was opened
     * to be read from cannot be written to (POSIX: EBADF). */
    if ((file->flags & KSYS_O_ACCMODE) == KSYS_O_RDONLY) {
        return KERR_BADF;
    }

    return vfs_write(&file->file, (const void *)(uintptr_t)buf, count);
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
    struct kfile *file = fd_get((int)fd);

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

    rc = vfs_stat((const char *)(uintptr_t)path, &stat);
    if (rc != 0) {
        return rc;
    }

    if (buf != 0) {
        memcpy((void *)(uintptr_t)buf, &stat, sizeof(stat));
    }

    return 0;
}

static k_ssize_t sys_lseek(ksys_arg_t fd, ksys_arg_t offset, ksys_arg_t whence, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get((int)fd);
    k_ssize_t position;

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (file == NULL) {
        return KERR_BADF;
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
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return vfs_unlink((const char *)(uintptr_t)path);
}

static k_ssize_t sys_mkdir(ksys_arg_t path, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                           ksys_arg_t a4, ksys_arg_t a5)
{
    K_UNUSED(a1);
    K_UNUSED(a2);
    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    return vfs_mkdir((const char *)(uintptr_t)path);
}

static k_ssize_t sys_readdir(ksys_arg_t fd, ksys_arg_t buf, ksys_arg_t count, ksys_arg_t a3,
                             ksys_arg_t a4, ksys_arg_t a5)
{
    struct kfile *file = fd_get((int)fd);
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

    if (buf != 0) {
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
    struct kfile *file = fd_get((int)fd);

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
 * NOT copied - the kernel of the board cannot do that - so the tasks share
 * it, like the flag CLONE_VM of Linux. The child has its own number, so
 * the program tells the child by the mark of its task.
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
    k_u32_t waited = 0;
    const k_u32_t limit = 5000; /* five seconds */

    K_UNUSED(a3);
    K_UNUSED(a4);
    K_UNUSED(a5);

    if (self == NULL) {
        return KERR_PERM;
    }

    /* Look for a child that has finished. */
    for (waited = 0; waited < limit; waited++) {
        k_size_t i;

        for (i = 0; i < TASK_MAX_TASKS; i++) {
            struct task *t = task_get_by_index(i);

            if (t == NULL || t->parent != self) {
                continue;
            }

            if (pid > 0 && (int)t->pid != (int)pid) {
                continue;
            }

            if (t->state == TASK_STATE_ZOMBIE) {
                child = t;
                break;
            }
        }

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
        *(int *)(uintptr_t)status = (int)child->exit_code;
    }

    {
        k_u32_t child_pid = child->pid;

        task_reap(child_pid);

        return (k_ssize_t)child_pid;
    }
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
