/*
 * Micro OS — the procfs.
 *
 * A virtual filesystem with the information about the system: the version of
 * the kernel, the time since the boot, the memory and the list of the tasks.
 * The files are generated when they are read, they are not stored anywhere.
 *
 *   /proc/version     the version of the kernel
 *   /proc/uptime      the time since the boot
 *   /proc/meminfo     the memory of the system
 *   /proc/tasks       the list of the tasks
 *   /proc/modules     the state of the modules
 *   /proc/self/       the task that reads the file
 *   /proc/<pid>/      the task with this number
 */
#include "procfs/procfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/kmm.h"
#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"
#include "kernel/vfs.h"

#define PROCFS_TEXT_MAX 2048
#define PROC_NAME_MAX 32

struct proc_file {
    const char *name;
    int (*read)(char *buf, k_size_t size);
};

/* --- the files of the root --- */

static int read_version(char *buf, k_size_t size)
{
    return snprintf(buf, size, "%s %s (%s)\n", kernel_name(), kernel_version(), kernel_build_id());
}

static int read_uptime(char *buf, k_size_t size)
{
    k_u64_t ms = k_sched_uptime_ms();

    return snprintf(buf, size, "up %llu s, %u ticks\n", (unsigned long long)(ms / 1000),
                    (unsigned)k_sched_ticks());
}

static int read_meminfo(char *buf, k_size_t size)
{
    struct vfs_statfs fs;
    int n;

    n = snprintf(buf, size,
                 "kernel heap: %u KB free of %u KB\n"
                 "psram: %u KB free of %u KB\n",
                 (unsigned)(k_port_heap_free() / 1024), (unsigned)(k_port_heap_total() / 1024),
                 (unsigned)(k_port_psram_free() / 1024), (unsigned)(k_port_psram_total() / 1024));

    if (vfs_statfs("/", &fs) == 0) {
        n += snprintf(buf + n, size - (k_size_t)n,
                      "filesystem: %u KB free of %u KB, %u inodes free\n",
                      (unsigned)(fs.free_blocks * fs.block_size / 1024),
                      (unsigned)(fs.total_blocks * fs.block_size / 1024), (unsigned)fs.free_inodes);
    }

    return n;
}

static int read_tasks(char *buf, k_size_t size)
{
    k_size_t count = task_count();
    k_size_t i;
    int n = 0;

    n += snprintf(buf + n, size - (k_size_t)n, "pid  prio  state     name\n");

    for (i = 0; i < count + 8; i++) {
        struct task *t = task_get_by_index(i);

        if (t == NULL) {
            continue;
        }

        n += snprintf(buf + n, size - (k_size_t)n, "%4u  %4u  %-8s  %s\n", (unsigned)t->pid,
                      (unsigned)t->prio, task_state_name(t->state), t->name);
    }

    return n;
}

static int read_modules(char *buf, k_size_t size)
{
    k_size_t count = kmod_count();
    k_size_t i;
    int n = 0;

    n += snprintf(buf + n, size - (k_size_t)n, "module     state     detail\n");

    for (i = 0; i < count; i++) {
        const struct kmod *mod = kmod_get(i);

        if (mod == NULL) {
            continue;
        }

        n += snprintf(buf + n, size - (k_size_t)n, "%-10s %-9s %s\n", mod->name,
                      kmod_state_name(mod->state), mod->info[0] != '\0' ? mod->info : mod->desc);
    }

    return n;
}

static const struct proc_file s_root_files[] = {
    {"version", read_version}, {"uptime", read_uptime},   {"meminfo", read_meminfo},
    {"tasks", read_tasks},     {"modules", read_modules},
};

static const k_size_t s_root_file_count = K_ARRAY_SIZE(s_root_files);

/* --- the files of a task --- */

/* The task the files of a directory belong to. */
static k_u32_t s_task_pid;

/* The point where the filesystem is mounted. */
static char s_point[PROC_NAME_MAX];

static int read_status(char *buf, k_size_t size)
{
    struct task *t = NULL;
    k_u32_t pid = 0;
    int n;

    if (s_task_pid != 0) {
        t = task_by_pid(s_task_pid);
        pid = s_task_pid;
    } else {
        struct task *self = task_self();

        if (self != NULL) {
            t = self;
            pid = self->pid;
        }
    }

    n = snprintf(buf, size, "Name:\t%s\nPid:\t%u\nState:\t%s\nPrio:\t%u\n",
                 (t != NULL) ? t->name : "?", (unsigned)pid,
                 (t != NULL) ? task_state_name(t->state) : "?", (t != NULL) ? t->prio : 0);

    if (t != NULL) {
        n += snprintf(buf + n, size - (k_size_t)n, "ExitCode:\t%d\n", (int)t->exit_code);
    }

    return n;
}

static const struct proc_file s_task_files[] = {
    {"status", read_status},
};

static const k_size_t s_task_file_count = K_ARRAY_SIZE(s_task_files);

/* --- the paths --- */

/* The path without the point of the mount. */
static const char *relative_path(const char *path)
{
    k_size_t len = strlen(s_point);

    if (strncmp(path, s_point, len) != 0) {
        return path;
    }

    path += len;
    while (*path == '/') {
        path++;
    }

    return path;
}

/* The name of the file without the directory. */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        return path;
    }

    return slash + 1;
}

/*
 * The name of the directory of the path: the last component of the parent.
 * For "/proc/self/status" it is "self", for "/proc/version" it is empty.
 */
static void dir_name(const char *path, char *out, k_size_t size)
{
    const char *slash;
    const char *start;
    k_size_t len;

    if (size == 0) {
        return;
    }

    out[0] = '\0';

    slash = strrchr(path, '/');
    if (slash == NULL) {
        return;
    }

    /* The last component of the parent. */
    start = slash;
    while (start > path && start[-1] != '/') {
        start--;
    }

    len = (k_size_t)(slash - start);
    if (len >= size) {
        len = size - 1;
    }

    memcpy(out, start, len);
    out[len] = '\0';
}

/* The number of the task of a directory: "self" or the number. */
static k_u32_t dir_task_pid(const char *dir)
{
    k_u32_t pid = 0;

    if (dir[0] == '\0' || strcmp(dir, "/") == 0) {
        return 0;
    }

    if (strcmp(dir, "self") == 0) {
        struct task *self = task_self();

        return (self != NULL) ? self->pid : 0;
    }

    /* The directory of a task is its number. */
    pid = (k_u32_t)strtoul(dir, NULL, 10);

    return pid;
}

static const struct proc_file *find_root_file(const char *name)
{
    k_size_t i;

    for (i = 0; i < s_root_file_count; i++) {
        if (strcmp(s_root_files[i].name, name) == 0) {
            return &s_root_files[i];
        }
    }

    return NULL;
}

static const struct proc_file *find_task_file(const char *name)
{
    k_size_t i;

    for (i = 0; i < s_task_file_count; i++) {
        if (strcmp(s_task_files[i].name, name) == 0) {
            return &s_task_files[i];
        }
    }

    return NULL;
}

/* --- the operations --- */

static int procfs_open(void *ctx, const char *path, int flags, struct vfs_file *out, k_size_t slot)
{
    char dir[PROC_NAME_MAX];
    const char *rel = relative_path(path);
    const char *name = base_name(rel);
    const struct proc_file *file = NULL;

    K_UNUSED(ctx);
    K_UNUSED(flags);
    K_UNUSED(slot);

    dir_name(rel, dir, sizeof(dir));
    s_task_pid = dir_task_pid(dir);

    if (s_task_pid != 0) {
        file = find_task_file(name);
    } else {
        file = find_root_file(name);
    }

    if (file == NULL) {
        return -2;
    }

    if (out->priv != NULL) {
        snprintf((char *)out->priv, PROC_NAME_MAX, "%s", name);
    }

    return 0;
}

static int procfs_read(void *ctx, struct vfs_file *file, void *buf, k_size_t size)
{
    const char *name = (file->priv != NULL) ? (const char *)file->priv : "";
    const struct proc_file *proc;
    char text[PROCFS_TEXT_MAX];
    int total;

    K_UNUSED(ctx);

    /* The name of the file alone does not say where it lives, the directory
     * of the path decides. */
    proc = (s_task_pid != 0) ? find_task_file(name) : find_root_file(name);

    if (proc == NULL) {
        return -2;
    }

    total = proc->read(text, sizeof(text) - 1);
    if (total < 0) {
        total = 0;
    }
    text[total] = '\0';

    if (file->offset >= (k_u64_t)total) {
        return 0; /* the end of the file */
    }

    if (file->offset + size > (k_u64_t)total) {
        size = (k_size_t)((k_u64_t)total - file->offset);
    }

    memcpy(buf, text + file->offset, size);
    file->offset += size;

    return (int)size;
}

static int procfs_list(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                       k_size_t *count)
{
    char dir[PROC_NAME_MAX];
    const char *rel = relative_path(path);
    k_size_t i;
    k_size_t found = 0;

    K_UNUSED(ctx);

    /*
     * The listing of the root has the files and the directories of the tasks,
     * the listing of a task has the files of the task.
     */
    dir_name(rel, dir, sizeof(dir));
    if (dir[0] == '\0') {
        /* The root: the name of the path is the task, if there is one. */
        s_task_pid = dir_task_pid(rel);
    } else {
        s_task_pid = dir_task_pid(dir);
    }

    if (s_task_pid != 0) {
        /* The files of a task. */
        for (i = 0; i < s_task_file_count && found < max; i++) {
            memset(&out[found], 0, sizeof(out[found]));
            snprintf(out[found].name, sizeof(out[found].name), "%s", s_task_files[i].name);
            out[found].type = 1;
            found++;
        }
    } else {
        /* The files of the root. */
        for (i = 0; i < s_root_file_count && found < max; i++) {
            memset(&out[found], 0, sizeof(out[found]));
            snprintf(out[found].name, sizeof(out[found].name), "%s", s_root_files[i].name);
            out[found].type = 1;
            found++;
        }

        /* The directory of the task that reads the file. */
        {
            struct task *self = task_self();

            if (self != NULL && found < max) {
                memset(&out[found], 0, sizeof(out[found]));
                snprintf(out[found].name, sizeof(out[found].name), "self");
                out[found].type = 2;
                found++;
            }
        }

        /* The directories of the tasks. */
        for (i = 0; i < TASK_MAX_TASKS && found < max; i++) {
            struct task *t = task_get_by_index(i);

            if (t == NULL) {
                continue;
            }

            memset(&out[found], 0, sizeof(out[found]));
            snprintf(out[found].name, sizeof(out[found].name), "%u", (unsigned)t->pid);
            out[found].type = 2;
            out[found].inode = t->pid;
            found++;
        }
    }

    if (count != NULL) {
        *count = found;
    }

    return 0;
}

static int procfs_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    char dir[PROC_NAME_MAX];
    const char *rel = relative_path(path);
    const char *name = base_name(rel);
    const struct proc_file *file;

    K_UNUSED(ctx);

    dir_name(rel, dir, sizeof(dir));
    s_task_pid = dir_task_pid(dir);

    file = (s_task_pid != 0) ? find_task_file(name) : find_root_file(name);
    if (file == NULL) {
        return -2;
    }

    memset(out, 0, sizeof(*out));
    out->type = 1;

    return 0;
}

static const struct vfs_operations g_operations = {
    .open = procfs_open,
    .read = procfs_read,
    .write = NULL,
    .close = NULL,
    .list = procfs_list,
    .stat = procfs_stat,
    .mkdir = NULL,
    .unlink = NULL,
    .strerror = NULL,
};

int procfs_register(const char *point)
{
    k_size_t len;

    if (point == NULL) {
        return -1;
    }

    len = strlen(point);
    if (len >= sizeof(s_point)) {
        return -1;
    }

    memcpy(s_point, point, len + 1);

    return vfs_mount(point, &g_operations, NULL);
}
