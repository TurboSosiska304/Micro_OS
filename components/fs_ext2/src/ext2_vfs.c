/*
 * Micro OS — the adapter between the ext2 reader and the VFS.
 *
 * The VFS hands a filesystem the whole path of the tree and the mount point
 * is the first part of it; the adapter takes that part away, the way procfs,
 * devfs and FAT do. What is left is the path inside the volume.
 *
 * The volume is read-only: the operations that would change it are left
 * empty and the VFS answers them with KERR_NOTSUP, which is the word of a
 * call that the filesystem does not have.
 */
#include "ext2_private.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/log.h"
#include "kernel/vfs.h"

/* One volume of this reader at a time, like MicroFS and FAT. */
static struct ext2_volume g_ext2;

static int volume_lock(struct ext2_volume *vol)
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

static k_u8_t entry_vfs_type(k_u8_t type)
{
    if (type == 2u) {
        return 2;
    }

    return (type == 1u || type == 0u) ? 1 : 3;
}

static int fill_dirent(struct ext2_volume *vol, const struct ext2_dirent *entry,
                       struct vfs_dirent *out)
{
    struct ext2_inode inode;

    copy_name(out->name, entry->name);
    out->inode = entry->inode;

    if (ext2_read_inode(vol, entry->inode, &inode) == 0) {
        out->type = ext2_inode_vfs_type(&inode);
        out->size = inode.size;
    } else {
        /* A damaged entry still has a name; the fields that could not be
         * read are not invented for it. */
        out->type = entry_vfs_type(entry->type);
        out->size = 0;
    }

    return 0;
}

static bool is_dot_entry(const char *name)
{
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

static int ext2_vfs_open(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    struct ext2_inode inode;
    k_u32_t number = 0;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    /* The volume takes no write and no new name: a descriptor for one is a
     * call this filesystem cannot keep. */
    if ((flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND)) != 0) {
        return KERR_NOTSUP;
    }

    relative = ext2_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_lookup(vol, relative, &inode, &number);
    if (rc != 0) {
        k_mutex_unlock(&vol->lock);
        return rc;
    }

    if (ext2_inode_is_dir(&inode)) {
        if ((flags & VFS_O_DIRECTORY) == 0) {
            rc = KERR_ISDIR;
        } else {
            out->inode = number;
            out->size = 0;
            out->offset = 0;
        }
    } else if ((flags & VFS_O_DIRECTORY) != 0) {
        rc = KERR_NOTDIR;
    } else if (!ext2_inode_is_file(&inode)) {
        /* A symbolic link, a device node or a fifo has no bytes for this
         * reader: following a link is a contract the VFS does not have. */
        rc = KERR_NOTSUP;
    } else {
        out->inode = number;
        out->size = inode.size;
        out->offset = 0;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_read(void *ctx, struct vfs_file *file, void *buf, k_size_t size)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    struct ext2_inode inode;
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

    rc = ext2_read_inode(vol, file->inode, &inode);
    if (rc == 0) {
        rc = ext2_read_file(vol, &inode, file->offset, buf, want, &got);
    }

    if (rc == 0 && got < want) {
        rc = KERR_IO;
    }

    if (rc == 0) {
        file->offset += got;
    }

    k_mutex_unlock(&vol->lock);

    return (rc != 0) ? rc : (int)got;
}

static int ext2_vfs_close(void *ctx, struct vfs_file *file)
{
    (void)ctx;
    (void)file;

    return 0;
}

static int ext2_vfs_statfs(void *ctx, struct vfs_statfs *out)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    out->total_blocks = vol->blocks_count;
    out->free_blocks = vol->free_blocks;
    out->total_inodes = vol->inodes_count;
    out->free_inodes = vol->free_inodes;
    out->block_size = vol->block_size;

    return 0;
}

static int ext2_vfs_list(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                         k_size_t *count)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    struct ext2_inode dir;
    struct ext2_dirent entry;
    k_u64_t pos = 0;
    int rc;

    if (vol == NULL || out == NULL || count == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    *count = 0;

    relative = ext2_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_lookup(vol, relative, &dir, NULL);
    if (rc == 0 && !ext2_inode_is_dir(&dir)) {
        rc = KERR_NOTDIR;
    }

    for (;;) {
        if (rc != 0) {
            break;
        }

        if (*count >= max) {
            rc = 0;
            break;
        }

        rc = ext2_dir_next(vol, &dir, &pos, &entry);
        if (rc < 0) {
            break;
        }
        if (rc == 0) {
            break;
        }

        if (is_dot_entry(entry.name)) {
            rc = 0;
            continue;
        }

        fill_dirent(vol, &entry, &out[*count]);
        (*count)++;
        rc = 0;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_readdir(void *ctx, struct vfs_file *file, struct vfs_dirent *out, k_size_t max,
                            k_size_t *count)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    struct ext2_inode dir;
    struct ext2_dirent entry;
    k_u64_t pos;
    int rc;

    if (vol == NULL || file == NULL || out == NULL || count == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    *count = 0;

    if (max == 0) {
        return 0;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_read_inode(vol, file->inode, &dir);
    if (rc != 0) {
        k_mutex_unlock(&vol->lock);
        return rc;
    }

    if (!ext2_inode_is_dir(&dir)) {
        k_mutex_unlock(&vol->lock);
        return KERR_NOTDIR;
    }

    pos = file->offset;

    for (;;) {
        if (*count >= max) {
            rc = 0;
            break;
        }

        rc = ext2_dir_next(vol, &dir, &pos, &entry);
        if (rc < 0) {
            break;
        }

        file->offset = pos;

        if (rc == 0) {
            rc = 0;
            break;
        }

        if (is_dot_entry(entry.name)) {
            continue;
        }

        fill_dirent(vol, &entry, &out[*count]);
        (*count)++;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    struct ext2_inode inode;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    relative = ext2_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_lookup(vol, relative, &inode, NULL);
    if (rc == 0) {
        out->inode = inode.number;
        out->type = ext2_inode_vfs_type(&inode);
        out->size = inode.size;
        out->mode = ext2_inode_mode(&inode);
        out->owner = inode.uid;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_statx(void *ctx, const char *path, struct vfs_statx *out)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    struct ext2_inode inode;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    relative = ext2_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_lookup(vol, relative, &inode, NULL);
    if (rc == 0) {
        out->inode = inode.number;
        out->type = ext2_inode_vfs_type(&inode);
        out->size = inode.size;
        out->mode = ext2_inode_mode(&inode);
        out->owner = inode.uid;
        out->mtime = inode.mtime;
        out->ctime = inode.ctime;
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_umount(void *ctx)
{
    ext2_volume_unmount((struct ext2_volume *)ctx);
    return 0;
}

static const struct vfs_operations g_ext2_operations = {
    .open = ext2_vfs_open,
    .read = ext2_vfs_read,
    .write = NULL, /* the volume is read-only */
    .close = ext2_vfs_close,
    .list = ext2_vfs_list,
    .readdir = ext2_vfs_readdir,
    .stat = ext2_vfs_stat,
    .statx = ext2_vfs_statx,
    .mkdir = NULL,
    .unlink = NULL,
    .chmod = NULL,
    .chown = NULL,
    .statfs = ext2_vfs_statfs,
    .strerror = ext2_strerror,
    .umount = ext2_vfs_umount,
};

int ext2_vfs_mount(const char *point, const char *device)
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
        kerror("ext2: there is no block device '%s'", device);
        return KERR_NOENT;
    }

    if (g_ext2.mounted) {
        if (strcmp(g_ext2.point, point) != 0) {
            return KERR_NOSPC;
        }
        ext2_volume_unmount(&g_ext2);
    }

    rc = ext2_volume_mount(&g_ext2, dev);
    if (rc != 0) {
        return rc;
    }

    memcpy(g_ext2.point, point, len + 1);

    rc = vfs_mount(point, &g_ext2_operations, &g_ext2);
    if (rc != 0) {
        ext2_volume_unmount(&g_ext2);
    }

    return rc;
}

int ext2_vfs_register_type(void)
{
    return vfs_register_type("ext2", ext2_vfs_mount);
}

const char *ext2_strerror(int error)
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
        return "operation not supported on a read-only ext2 volume";
    default:
        return "unknown error";
    }
}
