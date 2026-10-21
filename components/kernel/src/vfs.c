#include "kernel/vfs.h"

#include <string.h>

#include "kernel/log.h"

struct mount {
    char point[VFS_PATH_MAX];
    const struct vfs_operations *ops;
    void *ctx;
};

#define VFS_MAX_FILES 16
#define VFS_FILE_DATA_SIZE 64

static struct mount g_mounts[VFS_MAX_MOUNTS];
static k_size_t g_count;
static int g_last_error;

/* The private data of the open files, one slot per descriptor. */
static uint8_t g_file_data[VFS_MAX_FILES][VFS_FILE_DATA_SIZE];

void *vfs_file_data(k_size_t index)
{
    if (index >= VFS_MAX_FILES) {
        return NULL;
    }

    return g_file_data[index];
}

static void copy_str(char *dst, k_size_t size, const char *src)
{
    k_size_t len;

    if (size == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len >= size) {
        len = size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

int vfs_mount(const char *point, const struct vfs_operations *ops, void *ctx)
{
    struct mount *slot = NULL;
    k_size_t i;

    if (point == NULL || ops == NULL) {
        return -1;
    }

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_mounts[i].point, point) == 0) {
            /* The filesystem is already mounted, replace it. */
            g_mounts[i].ops = ops;
            g_mounts[i].ctx = ctx;
            return 0;
        }
    }

    if (g_count >= VFS_MAX_MOUNTS) {
        kerror("vfs: there is no room for another filesystem");
        return -1;
    }

    slot = &g_mounts[g_count];
    copy_str(slot->point, sizeof(slot->point), point);
    slot->ops = ops;
    slot->ctx = ctx;
    g_count++;

    kinfo("vfs: '%s' is mounted", slot->point);

    return 0;
}

int vfs_umount(const char *point)
{
    k_size_t i;

    if (point == NULL) {
        return -1;
    }

    for (i = 0; i < g_count; i++) {
        if (strcmp(g_mounts[i].point, point) == 0) {
            for (; i + 1 < g_count; i++) {
                g_mounts[i] = g_mounts[i + 1];
            }
            g_count--;
            return 0;
        }
    }

    return -1;
}

k_size_t vfs_mount_count(void)
{
    return g_count;
}

const char *vfs_mount_name(k_size_t index)
{
    if (index >= g_count) {
        return NULL;
    }

    return g_mounts[index].point;
}

/*
 * Find the filesystem of a path: the mount with the longest matching
 * prefix. The rest of the path belongs to the filesystem.
 */
static const struct mount *resolve(const char *path, const char **relative)
{
    const struct mount *best = NULL;
    k_size_t best_len = 0;
    k_size_t i;

    if (path == NULL) {
        return NULL;
    }

    for (i = 0; i < g_count; i++) {
        k_size_t len = strlen(g_mounts[i].point);

        if (strncmp(path, g_mounts[i].point, len) != 0) {
            continue;
        }

        /*
         * The point has to be a whole directory of the path. The root is
         * the prefix of every path, so it always matches.
         */
        if (len > 1 && path[len] != '\0' && path[len] != '/') {
            continue;
        }

        if (best == NULL || len > best_len) {
            best = &g_mounts[i];
            best_len = len;
        }
    }

    if (best == NULL) {
        return NULL;
    }

    if (relative != NULL) {
        const char *rest = path + best_len;

        if (*rest == '\0') {
            *relative = "/";
        } else if (*rest == '/') {
            *relative = rest;
        } else {
            *relative = rest;
        }
    }

    return best;
}

int vfs_open(const char *path, int flags, struct vfs_file *out, k_size_t slot)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL || out == NULL) {
        g_last_error = -2;
        return -2;
    }

    out->priv = vfs_file_data(slot);
    out->ops = m->ops;
    out->fs = m->ctx;

    rc = m->ops->open(m->ctx, path, flags, out, slot);
    g_last_error = rc;

    if (rc != 0) {
        out->ops = NULL;
        out->fs = NULL;
    }

    return rc;
}

int vfs_read(struct vfs_file *file, void *buf, k_size_t size)
{
    int rc;

    if (file == NULL || file->ops == NULL || file->ops->read == NULL) {
        return -1;
    }

    rc = file->ops->read(file->fs, file, buf, size);
    g_last_error = rc;

    return rc;
}

int vfs_write(struct vfs_file *file, const void *buf, k_size_t size)
{
    int rc;

    if (file == NULL || file == NULL || file->ops == NULL || file->ops->write == NULL) {
        return -1;
    }

    rc = file->ops->write(file->fs, file, buf, size);
    g_last_error = rc;

    return rc;
}

int vfs_close(struct vfs_file *file)
{
    int rc;

    if (file == NULL || file->ops == NULL || file->ops->close == NULL) {
        return -1;
    }

    rc = file->ops->close(file->fs, file);
    g_last_error = rc;

    return rc;
}

int vfs_list(const char *path, struct vfs_dirent *out, k_size_t max, k_size_t *count)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = -2;
        return -2;
    }

    if (m->ops->list == NULL) {
        g_last_error = -1;
        return -1;
    }

    rc = m->ops->list(m->ctx, path, out, max, count);
    g_last_error = rc;

    return rc;
}

int vfs_stat(const char *path, struct vfs_stat *out)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = -2;
        return -2;
    }

    if (m->ops->stat == NULL) {
        g_last_error = -1;
        return -1;
    }

    rc = m->ops->stat(m->ctx, path, out);
    g_last_error = rc;

    return rc;
}

int vfs_mkdir(const char *path)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = -2;
        return -2;
    }

    if (m->ops->mkdir == NULL) {
        g_last_error = -1;
        return -1;
    }

    rc = m->ops->mkdir(m->ctx, path);
    g_last_error = rc;

    return rc;
}

int vfs_statfs(const char *path, struct vfs_statfs *out)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL || out == NULL) {
        g_last_error = -2;
        return -2;
    }

    if (m->ops->statfs == NULL) {
        g_last_error = -1;
        return -1;
    }

    rc = m->ops->statfs(m->ctx, out);
    g_last_error = rc;

    return rc;
}

int vfs_unlink(const char *path)
{
    const struct mount *m = resolve(path, NULL);
    int rc;

    if (m == NULL) {
        g_last_error = -2;
        return -2;
    }

    if (m->ops->unlink == NULL) {
        g_last_error = -1;
        return -1;
    }

    rc = m->ops->unlink(m->ctx, path);
    g_last_error = rc;

    return rc;
}

const char *vfs_strerror(int error)
{
    k_size_t i;

    for (i = 0; i < g_count; i++) {
        if (g_mounts[i].ops->strerror != NULL) {
            return g_mounts[i].ops->strerror(error);
        }
    }

    if (error == -2) {
        return "no such file or directory";
    }

    return "unknown error";
}
