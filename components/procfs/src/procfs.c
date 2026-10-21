/*
 * Micro OS — the procfs.
 *
 * A virtual filesystem with the information about the system: the version of
 * the kernel, the time since the boot, the memory and the list of the tasks.
 * The files are generated when they are read, they are not stored anywhere.
 */
#include "procfs/procfs.h"

#include <stdio.h>
#include <string.h>

#include "kernel/kmm.h"
#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"
#include "kernel/vfs.h"

#define PROCFS_MAX 128
#define PROC_NAME_MAX 32
#define PROCFS_TEXT_MAX 1024

struct proc_file {
    const char *name;
    int (*read)(char *buf, k_size_t size);
};

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
                      kmod_state_name(mod->state), mod->info);
    }

    return n;
}

static const struct proc_file s_files[] = {
    {"version", read_version}, {"uptime", read_uptime},   {"meminfo", read_meminfo},
    {"tasks", read_tasks},     {"modules", read_modules},
};

static const k_size_t s_file_count = K_ARRAY_SIZE(s_files);

static const struct proc_file *find_file(const char *name)
{
    k_size_t i;

    for (i = 0; i < s_file_count; i++) {
        if (strcmp(s_files[i].name, name) == 0) {
            return &s_files[i];
        }
    }

    return NULL;
}

/* The name of the file without the directory of the mount. */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    if (slash == NULL) {
        return path;
    }

    return slash + 1;
}

static int procfs_open(void *ctx, const char *path, int flags, struct vfs_file *out, k_size_t slot)
{
    const char *name = base_name(path);
    const struct proc_file *file = find_file(name);

    K_UNUSED(ctx);
    K_UNUSED(flags);
    K_UNUSED(slot);

    if (file == NULL) {
        return -2;
    }

    /* The name of the file is remembered in the descriptor. */
    if (out->priv != NULL) {
        snprintf((char *)out->priv, PROC_NAME_MAX, "%s", name);
    }

    return 0;
}

static int procfs_read(void *ctx, struct vfs_file *file, void *buf, k_size_t size)
{
    const char *name = (file->priv != NULL) ? (const char *)file->priv : "";
    const struct proc_file *proc = find_file(name);
    char text[PROCFS_TEXT_MAX];
    int total;

    K_UNUSED(ctx);

    if (proc == NULL) {
        return -2;
    }

    /*
     * The file is generated when it is read, but the reader has to be able
     * to reach the end of it: the offset of the descriptor says how much of
     * the text was already read.
     */
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
    k_size_t i;
    k_size_t found = 0;

    K_UNUSED(ctx);
    K_UNUSED(path);

    for (i = 0; i < s_file_count && found < max; i++) {
        memset(&out[found], 0, sizeof(out[found]));
        snprintf(out[found].name, sizeof(out[found].name), "%s", s_files[i].name);
        out[found].type = 1; /* a file */
        out[found].size = 0;
        found++;
    }

    if (count != NULL) {
        *count = found;
    }

    return 0;
}

static int procfs_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    const struct proc_file *file = find_file(base_name(path));

    K_UNUSED(ctx);

    if (file == NULL) {
        return -2;
    }

    memset(out, 0, sizeof(*out));
    out->type = 1;
    out->size = 0;

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
    return vfs_mount(point, &g_operations, NULL);
}
