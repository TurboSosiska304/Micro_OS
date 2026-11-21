/*
 * Micro OS — the adapter between the FAT reader and the VFS.
 *
 * The VFS hands a filesystem the whole path of the tree and the mount point
 * is the first part of it; the adapter takes that part away, the way procfs
 * and devfs do. What is left is the path inside the volume.
 */
#include "fat_private.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/vfs.h"

/*
 * One volume of this reader at a time. The board has one card and a second
 * FAT volume on the same machine is a question of a second card, not of a
 * bigger table; MicroFS keeps one volume for the same reason.
 */
static struct fat_volume g_fat;

const char *fat_relative(const struct fat_volume *vol, const char *path)
{
    k_size_t len;

    if (vol == NULL || path == NULL) {
        return NULL;
    }

    len = strlen(vol->point);
    if (strncmp(path, vol->point, len) != 0) {
        return NULL;
    }

    if (path[len] == '\0') {
        return "/";
    }

    if (path[len] != '/') {
        return NULL;
    }

    return path + len;
}

static int volume_lock(struct fat_volume *vol)
{
    return (k_mutex_lock(&vol->lock) == 0) ? 0 : KERR_NOMEM;
}

/*
 * The name of a dirent is a window of VFS_NAME_MAX bytes. A longer name is
 * cut at a character boundary; the lookup of a path still compares the
 * whole name, so a file is reachable even when its listing is short.
 */
static void copy_name(char *dst, const char *src)
{
    k_size_t len = strlen(src);

    if (len > VFS_NAME_MAX) {
        len = VFS_NAME_MAX;
        while (len > 0 && ((unsigned char)src[len] & 0xC0u) == 0x80u) {
            len--;
        }
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int fat_vfs_open(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    const char *relative;
    struct fat_entry entry;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if ((flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) != 0) {
        return KERR_NOTSUP;
    }

    relative = fat_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = fat_lookup(vol, relative, &entry);
    if (rc == 0) {
        if (entry.dir) {
            if ((flags & VFS_O_DIRECTORY) == 0) {
                rc = KERR_ISDIR;
            } else {
                out->inode = entry.cluster;
                out->size = 0;
            }
        } else {
            if ((flags & VFS_O_DIRECTORY) != 0) {
                rc = KERR_NOTDIR;
            } else {
                out->inode = entry.cluster;
                out->size = entry.size;
            }
        }
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int fat_vfs_read(void *ctx, struct vfs_file *file, void *buf, k_size_t size)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    k_size_t want;
    k_size_t got = 0;
    int rc;

    if (vol == NULL || file == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (buf == NULL && size != 0) {
        return KERR_INVAL;
    }

    if (file->offset >= file->size) {
        return 0;
    }

    want = size;
    if ((k_u64_t)want > file->size - file->offset) {
        want = (k_size_t)(file->size - file->offset);
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = fat_read_chain(vol, file->inode, file->offset, buf, want, &got);
    if (rc == 0 && got < want) {
        rc = KERR_IO;
    }

    if (rc == 0) {
        file->offset += got;
    }

    k_mutex_unlock(&vol->lock);

    return (rc != 0) ? rc : (int)got;
}

static int fat_vfs_close(void *ctx, struct vfs_file *file)
{
    K_UNUSED(ctx);
    K_UNUSED(file);

    return 0;
}

static void fill_dirent(struct vfs_dirent *out, const struct fat_entry *entry)
{
    copy_name(out->name, entry->name);
    out->inode = entry->cluster;
    out->type = entry->dir ? 2 : 1;
    out->size = entry->size;
}

static int fat_vfs_readdir(void *ctx, struct vfs_file *file, struct vfs_dirent *out, k_size_t max,
                           k_size_t *count)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    struct fat_entry entry;
    k_u32_t pos;
    int rc;

    if (vol == NULL || file == NULL || out == NULL || count == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    *count = 0;

    if ((file->flags & VFS_O_DIRECTORY) == 0) {
        return KERR_NOTDIR;
    }

    if (max == 0) {
        return 0;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    pos = (k_u32_t)file->offset;

    for (;;) {
        if (*count >= max) {
            rc = 0;
            break;
        }

        rc = fat_scan_next(vol, file->inode, &pos, &entry);
        if (rc < 0) {
            break;
        }

        file->offset = pos;

        if (rc == 0) {
            rc = 0;
            break;
        }

        fill_dirent(&out[*count], &entry);
        (*count)++;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int fat_vfs_list(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                        k_size_t *count)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    const char *relative;
    struct fat_entry dir;
    struct fat_entry entry;
    k_u32_t pos = 0;
    int rc;

    if (vol == NULL || out == NULL || count == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    *count = 0;

    relative = fat_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = fat_lookup(vol, relative, &dir);
    if (rc != 0) {
        k_mutex_unlock(&vol->lock);
        return rc;
    }

    if (!dir.dir) {
        k_mutex_unlock(&vol->lock);
        return KERR_NOTDIR;
    }

    while (*count < max) {
        rc = fat_scan_next(vol, dir.cluster, &pos, &entry);
        if (rc < 0) {
            break;
        }
        if (rc == 0) {
            rc = 0;
            break;
        }
        fill_dirent(&out[*count], &entry);
        (*count)++;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int fat_vfs_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    const char *relative;
    struct fat_entry entry;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    relative = fat_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = fat_lookup(vol, relative, &entry);
    if (rc == 0) {
        out->inode = entry.cluster;
        out->type = entry.dir ? 2 : 1;
        out->size = entry.size;
        out->mode = 0; /* FAT has no modes of its own */
        out->owner = 0;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int fat_vfs_umount(void *ctx)
{
    fat_volume_unmount((struct fat_volume *)ctx);
    return 0;
}

static const struct vfs_operations g_fat_operations = {
    .open = fat_vfs_open,
    .read = fat_vfs_read,
    .close = fat_vfs_close,
    .list = fat_vfs_list,
    .readdir = fat_vfs_readdir,
    .stat = fat_vfs_stat,
    .strerror = fat_strerror,
    .umount = fat_vfs_umount,
};

int fat_vfs_mount(const char *point, const char *device)
{
    const struct k_blockdev *dev;
    k_size_t len;
    int rc;

    if (point == NULL || device == NULL) {
        return KERR_INVAL;
    }

    len = strlen(point);
    if (len == 0 || len >= VFS_PATH_MAX) {
        return KERR_NAMETOOLONG;
    }

    dev = k_blockdev_find(device);
    if (dev == NULL) {
        kerror("fat: there is no block device '%s'", device);
        return KERR_NOENT;
    }

    if (g_fat.mounted) {
        if (strcmp(g_fat.point, point) != 0) {
            return KERR_NOSPC;
        }
        fat_volume_unmount(&g_fat);
    }

    rc = fat_volume_mount(&g_fat, dev);
    if (rc != 0) {
        return rc;
    }

    memcpy(g_fat.point, point, len + 1);

    rc = vfs_mount(point, &g_fat_operations, &g_fat);
    if (rc != 0) {
        fat_volume_unmount(&g_fat);
    }

    return rc;
}

int fat_vfs_register_type(void)
{
    return vfs_register_type("fat32", fat_vfs_mount);
}

const char *fat_strerror(int error)
{
    switch (error) {
    case KERR_NONE:
        return "no error";
    case KERR_NOENT:
        return "no such file or directory";
    case KERR_IO:
        return "input/output error";
    case KERR_INVAL:
        return "invalid argument";
    case KERR_NOTDIR:
        return "not a directory";
    case KERR_ISDIR:
        return "is a directory";
    case KERR_NAMETOOLONG:
        return "the name is longer than the window";
    case KERR_NOTSUP:
        return "operation not supported on a read-only FAT volume";
    default:
        return "unknown error";
    }
}
