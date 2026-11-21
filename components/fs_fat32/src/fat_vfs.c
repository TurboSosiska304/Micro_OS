/*
 * Micro OS — the adapter between the FAT filesystem and the VFS.
 *
 * The VFS hands a filesystem the whole path of the tree and the mount point
 * is the first part of it; the adapter takes that part away, the way procfs
 * and devfs do. What is left is the path inside the volume.
 *
 * A writable descriptor is the place where the two halves meet: the open
 * call remembers the directory entry of the file, and every write puts the
 * new first cluster and the new size back into it before it returns.
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

static void fill_file(struct vfs_file *out, const struct fat_entry *entry, k_u32_t dir_cluster,
                      k_u32_t pos)
{
    struct fat_file *file = (struct fat_file *)out->priv;

    out->inode = entry->cluster;
    out->size = entry->size;

    if (file == NULL) {
        return;
    }

    file->dir_cluster = dir_cluster;
    file->dir_pos = pos;
    file->first_cluster = entry->cluster;
    file->dir_first = entry->cluster;
    file->size = entry->size;
}

static int fat_vfs_open(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    const char *relative;
    struct fat_entry entry;
    struct fat_entry parent;
    char leaf[FAT_NAME_MAX + 1];
    k_u32_t dir_cluster = FAT_POS_NONE;
    k_u32_t pos = FAT_POS_NONE;
    bool writable = (flags & (VFS_O_WRONLY | VFS_O_RDWR)) != 0;
    bool mutated = false;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (writable && out->priv == NULL) {
        return KERR_INVAL; /* the descriptor needs the place to keep the entry */
    }

    relative = fat_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = fat_lookup_pos(vol, relative, &entry, &dir_cluster, &pos);

    if (rc == KERR_NOENT && (flags & VFS_O_CREAT) != 0) {
        rc = fat_lookup_parent(vol, relative, &parent, leaf, sizeof(leaf));
        if (rc == 0) {
            rc = fat_dir_create(vol, parent.cluster, leaf, FAT_ATTR_ARCHIVE, 0, 0, &pos);
        }
        if (rc == 0) {
            memset(&entry, 0, sizeof(entry));
            entry.attr = FAT_ATTR_ARCHIVE;
            dir_cluster = parent.cluster;
            mutated = true;
        }
    }

    if (rc != 0) {
        k_mutex_unlock(&vol->lock);
        return rc;
    }

    if (entry.dir) {
        if ((flags & VFS_O_DIRECTORY) == 0) {
            rc = KERR_ISDIR;
        } else if (writable) {
            rc = KERR_ISDIR;
        } else {
            out->inode = entry.cluster;
            out->size = 0;
            out->offset = 0;
        }
    } else if ((flags & VFS_O_DIRECTORY) != 0) {
        rc = KERR_NOTDIR;
    } else {
        if (writable && (flags & VFS_O_TRUNC) != 0) {
            /* The entry forgets the chain before the chain is freed: a
             * failure between the two leaves clusters nobody names (a
             * checker's job), never an entry that points at a free map. */
            rc = fat_dir_set(vol, dir_cluster, pos, entry.cluster, 0, 0);
            if (rc == 0 && entry.cluster >= 2u) {
                rc = fat_chain_free(vol, entry.cluster);
            }
            if (rc == 0) {
                entry.cluster = 0;
                entry.size = 0;
                mutated = true;
            }
        }

        if (rc == 0) {
            if (writable && (out->priv == NULL || pos == FAT_POS_NONE)) {
                rc = KERR_INVAL;
            } else {
                fill_file(out, &entry, dir_cluster, pos);
                out->offset = ((flags & VFS_O_APPEND) != 0) ? entry.size : 0;
            }
        }
    }

    if (rc == 0 && mutated) {
        rc = fat_flush(vol);
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

static int fat_vfs_write(void *ctx, struct vfs_file *file, const void *buf, k_size_t size)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    struct fat_file *state;
    k_u64_t start;
    k_u64_t end;
    k_u32_t need;
    k_size_t done = 0;
    int rc;

    if (vol == NULL || file == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if ((file->flags & (VFS_O_WRONLY | VFS_O_RDWR)) == 0) {
        return KERR_BADF;
    }

    if (buf == NULL && size != 0) {
        return KERR_INVAL;
    }

    if (size == 0) {
        return 0;
    }

    state = (struct fat_file *)file->priv;
    if (state == NULL) {
        return KERR_INVAL;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    if ((file->flags & VFS_O_APPEND) != 0) {
        file->offset = state->size;
    }

    start = file->offset;
    end = start + size;

    if (end > 0xFFFFFFFFu) {
        rc = KERR_NOSPC; /* a FAT file stops below 4 GiB */
        goto out;
    }

    need = (k_u32_t)((end + vol->cluster_bytes - 1u) / vol->cluster_bytes);
    rc = fat_chain_extend(vol, &state->first_cluster, need);
    if (rc != 0) {
        goto out;
    }

    rc = fat_write_chain(vol, state->first_cluster, start, buf, size, &done);
    if (rc != 0) {
        goto out;
    }

    if (start + done > state->size) {
        state->size = (k_u32_t)(start + done);
    }

    rc = fat_dir_set(vol, state->dir_cluster, state->dir_pos, state->dir_first,
                     state->first_cluster, state->size);
    if (rc != 0) {
        goto out;
    }
    state->dir_first = state->first_cluster;

    file->offset = start + done;
    file->size = state->size;
    file->inode = state->first_cluster;

    rc = fat_flush(vol);
    if (rc == 0) {
        rc = (int)done;
    }

out:
    k_mutex_unlock(&vol->lock);

    return rc;
}

static int fat_vfs_close(void *ctx, struct vfs_file *file)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    int rc;

    /* Every change reached the medium before the call returned; the close
     * only asks the device to write through the cache it may hold. */
    if (vol == NULL || file == NULL || !vol->mounted) {
        return 0;
    }

    if ((file->flags & (VFS_O_WRONLY | VFS_O_RDWR)) == 0) {
        return 0;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = fat_flush(vol);
    k_mutex_unlock(&vol->lock);

    return rc;
}

static int fat_vfs_mkdir(void *ctx, const char *path)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    const char *relative;
    struct fat_entry existing;
    struct fat_entry parent;
    char leaf[FAT_NAME_MAX + 1];
    k_u32_t cluster = 0;
    int rc;

    if (vol == NULL || !vol->mounted) {
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

    rc = fat_lookup(vol, relative, &existing);
    if (rc == 0) {
        rc = KERR_EXIST;
        goto out;
    }
    if (rc != KERR_NOENT) {
        goto out;
    }

    rc = fat_lookup_parent(vol, relative, &parent, leaf, sizeof(leaf));
    if (rc != 0) {
        goto out;
    }
    if (!parent.dir) {
        rc = KERR_NOTDIR;
        goto out;
    }

    rc = fat_alloc_cluster(vol, &cluster);
    if (rc != 0) {
        goto out;
    }

    rc = fat_zero_cluster(vol, cluster);
    if (rc != 0) {
        (void)fat_chain_free(vol, cluster);
        goto out;
    }

    rc = fat_dir_create(vol, parent.cluster, leaf, FAT_ATTR_DIR, cluster, 0, NULL);
    if (rc != 0) {
        (void)fat_chain_free(vol, cluster);
        goto out;
    }

    rc = fat_dir_init(vol, cluster, (parent.cluster >= 2u) ? parent.cluster : 0);
    if (rc != 0) {
        goto out;
    }

    rc = fat_flush(vol);

out:
    k_mutex_unlock(&vol->lock);

    return rc;
}

static int fat_vfs_unlink(void *ctx, const char *path)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    const char *relative;
    int rc;

    if (vol == NULL || !vol->mounted) {
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

    rc = fat_dir_remove(vol, relative);
    if (rc == 0) {
        rc = fat_flush(vol);
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int fat_vfs_statfs(void *ctx, struct vfs_statfs *out)
{
    struct fat_volume *vol = (struct fat_volume *)ctx;
    k_u32_t free_clusters = 0;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = fat_clusters_free(vol, &free_clusters);
    if (rc == 0) {
        out->total_blocks = vol->cluster_count;
        out->free_blocks = free_clusters;
        out->total_inodes = 0; /* a FAT volume has no table of inodes */
        out->free_inodes = 0;
        out->block_size = vol->cluster_bytes;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
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
    .write = fat_vfs_write,
    .close = fat_vfs_close,
    .list = fat_vfs_list,
    .readdir = fat_vfs_readdir,
    .stat = fat_vfs_stat,
    .mkdir = fat_vfs_mkdir,
    .unlink = fat_vfs_unlink,
    .statfs = fat_vfs_statfs,
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

/*
 * The volume of a point or of a device is checked where it lives: a volume
 * that is mounted is read under its own lock, a device that is not is
 * mounted for the length of the walk and forgotten again. The device of a
 * mounted volume is the same medium, so the mounted copy answers for it;
 * opening a second reader on it would read the same bytes through a second
 * cache and no lock. A repair uses the same walk, with the writes of the
 * repair pass in front of the check.
 */
static int check_or_repair(const char *what, struct fat_check_report *report, bool write)
{
    const struct k_blockdev *dev;
    struct fat_volume *vol;
    int rc;

    if (what == NULL || what[0] == '\0' || report == NULL) {
        return KERR_INVAL;
    }

    memset(report, 0, sizeof(*report));

    if (g_fat.mounted && strcmp(what, g_fat.point) == 0) {
        vol = &g_fat;
    } else if (what[0] == '/') {
        /* A point that is not the volume of this reader: another
         * filesystem is mounted there, or nothing is. */
        return KERR_INVAL;
    } else {
        dev = k_blockdev_find(what);
        if (dev == NULL) {
            return KERR_NOENT;
        }

        if (!g_fat.mounted || g_fat.dev != dev) {
            return fat_check_device(dev, report, write);
        }

        vol = &g_fat;
    }

    rc = (k_mutex_lock(&vol->lock) == 0) ? 0 : KERR_NOMEM;
    if (rc != 0) {
        return rc;
    }

    rc = fat_check_volume(vol, report, write);

    k_mutex_unlock(&vol->lock);

    return rc;
}

int fat_check(const char *what, struct fat_check_report *report)
{
    return check_or_repair(what, report, false);
}

int fat_repair(const char *what, struct fat_check_report *report, bool write)
{
    return check_or_repair(what, report, write);
}

const char *fat_strerror(int error)
{
    switch (error) {
    case KERR_NONE:
        return "no error";
    case KERR_NOENT:
        return "no such file or directory";
    case KERR_EXIST:
        return "the object is already there";
    case KERR_IO:
        return "input/output error";
    case KERR_INVAL:
        return "invalid argument";
    case KERR_BADF:
        return "the file is not open for writing";
    case KERR_NOTDIR:
        return "not a directory";
    case KERR_ISDIR:
        return "is a directory";
    case KERR_NOTEMPTY:
        return "the directory is not empty";
    case KERR_NOSPC:
        return "no space left on the volume";
    case KERR_NAMETOOLONG:
        return "the name is longer than the window";
    case KERR_NOTSUP:
        return "operation not supported on a FAT volume";
    default:
        return "unknown error";
    }
}
