#include "kernel/ksys.h"

#include <string.h>

#include "kernel/errno.h"
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

    rc = vfs_open(name, (int)flags, &file->file, (k_size_t)fd);
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
