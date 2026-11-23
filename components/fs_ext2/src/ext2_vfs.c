/*
 * Micro OS — the adapter between ext2 and the VFS.
 *
 * The VFS hands a filesystem the whole path of the tree and the mount point
 * is the first part of it; the adapter takes that part away, the way procfs,
 * devfs and FAT do. What is left is the path inside the volume.
 *
 * The operations that change the volume keep the lock of the volume for the
 * whole call: the walk of a name and the blocks it writes are one state. A
 * call of this filesystem never allocates the memory of the kernel, so a
 * write of a root works on the board and on the machine of QEMU alike.
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

/*
 * The counters of the free blocks and of the free inodes of the superblock
 * are what e2fsck of the world compares with the bitmaps of the groups. The
 * volume of this machine is never unmounted - it is the root - so nothing
 * would write them out, and a sound image of a session would come to a
 * person as an image whose superblock lies. Every operation that allocates
 * or frees settles the volume when it is done: a close (the moment a file
 * says its bytes are there), a create, a directory, a removal and a cut.
 */
static int ext2_settle(struct ext2_volume *vol)
{
    return ext2_volume_flush(vol);
}

static int ext2_vfs_open(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    struct ext2_inode inode;
    k_u32_t number = 0;
    bool writable;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if ((flags & ~(VFS_O_WRONLY | VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC | VFS_O_APPEND |
                   VFS_O_DIRECTORY)) != 0) {
        return KERR_INVAL;
    }

    if ((flags & VFS_O_CREAT) != 0 && (flags & VFS_O_DIRECTORY) != 0) {
        return KERR_INVAL; /* a directory is made by mkdir, not by open */
    }

    writable = (flags & (VFS_O_WRONLY | VFS_O_RDWR)) != 0;

    relative = ext2_relative(vol, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_lookup(vol, relative, &inode, &number);

    if (rc == KERR_NOENT && (flags & VFS_O_CREAT) != 0) {
        rc = ext2_create_file(vol, relative, EXT2_MODE_FILE, &number);
        if (rc == 0) {
            rc = ext2_read_inode(vol, number, &inode);
        }

        if (rc == 0) {
            rc = ext2_settle(vol);
        }
    }

    if (rc != 0) {
        k_mutex_unlock(&vol->lock);
        return rc;
    }

    if (ext2_inode_is_dir(&inode)) {
        if ((flags & VFS_O_DIRECTORY) == 0) {
            rc = KERR_ISDIR;
        } else if (writable || (flags & VFS_O_TRUNC) != 0) {
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
         * filesystem: following a link is a contract the VFS does not
         * have. */
        rc = KERR_NOTSUP;
    } else {
        if ((flags & VFS_O_TRUNC) != 0) {
            rc = ext2_truncate_inode(vol, &inode, 0);
        }

        if (rc == 0) {
            out->inode = number;
            out->size = inode.size;
            out->offset = ((flags & VFS_O_APPEND) != 0) ? inode.size : 0;
        }
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
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    int rc;

    (void)file;

    if (vol == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_settle(vol);

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_write(void *ctx, struct vfs_file *file, const void *buf, k_size_t size)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    struct ext2_inode inode;
    k_size_t got = 0;
    k_u64_t offset;
    int rc;

    if (vol == NULL || file == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (buf == NULL && size != 0) {
        return KERR_INVAL;
    }

    if ((file->flags & (VFS_O_WRONLY | VFS_O_RDWR)) == 0) {
        return KERR_BADF; /* a descriptor of a reader has no write behind it */
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_read_inode(vol, file->inode, &inode);
    if (rc == 0) {
        offset = ((file->flags & VFS_O_APPEND) != 0) ? inode.size : file->offset;

        rc = ext2_file_write(vol, &inode, offset, buf, size, &got);

        if (got > 0) {
            file->offset = offset + got;
            file->size = inode.size;
        }
    }

    k_mutex_unlock(&vol->lock);

    return (rc != 0) ? rc : (int)got;
}

static int ext2_vfs_truncate_file(void *ctx, struct vfs_file *file, k_u64_t size)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    struct ext2_inode inode;
    int rc;

    if (vol == NULL || file == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if ((file->flags & (VFS_O_WRONLY | VFS_O_RDWR)) == 0) {
        return KERR_BADF;
    }

    /* A file cannot be larger than the volume that holds it. The growth of
     * ext2 is a hole, so without this check a number nobody can write would
     * still become the size of the inode. */
    if (size > (k_u64_t)vol->blocks_count * (k_u64_t)vol->block_size) {
        return KERR_NOSPC;
    }

    rc = volume_lock(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_read_inode(vol, file->inode, &inode);
    if (rc == 0) {
        rc = ext2_truncate_inode(vol, &inode, size);
    }

    if (rc == 0) {
        file->size = size;
        rc = ext2_settle(vol);
    }

    k_mutex_unlock(&vol->lock);

    return rc;
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

static int ext2_vfs_mkdir(void *ctx, const char *path)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    int rc;

    if (vol == NULL || path == NULL || !vol->mounted) {
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

    rc = ext2_make_dir(vol, relative, EXT2_MODE_DIR);

    if (rc == 0) {
        rc = ext2_settle(vol);
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_unlink(void *ctx, const char *path)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    int rc;

    if (vol == NULL || path == NULL || !vol->mounted) {
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

    rc = ext2_remove(vol, relative);

    if (rc == 0) {
        rc = ext2_settle(vol);
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_chmod(void *ctx, const char *path, k_u32_t mode)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    struct ext2_inode inode;
    int rc;

    if (vol == NULL || path == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if ((mode & ~EXT2_MODE_PERM) != 0u) {
        return KERR_INVAL; /* the mode is the nine bits and the three of setid */
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
        inode.mode = (k_u16_t)((inode.mode & ~EXT2_MODE_PERM) | (mode & EXT2_MODE_PERM));
        inode.ctime = ext2_now();
        rc = ext2_inode_write(vol, &inode, true);
    }

    k_mutex_unlock(&vol->lock);

    return rc;
}

static int ext2_vfs_chown(void *ctx, const char *path, k_u32_t owner)
{
    struct ext2_volume *vol = (struct ext2_volume *)ctx;
    const char *relative;
    struct ext2_inode inode;
    int rc;

    if (vol == NULL || path == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (owner > 0xFFFFu) {
        return KERR_INVAL; /* the owner of an ext2 record is 16 bits */
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
        inode.uid = (k_u16_t)owner;
        inode.ctime = ext2_now();
        rc = ext2_inode_write(vol, &inode, true);
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
    .write = ext2_vfs_write,
    .truncate = ext2_vfs_truncate_file,
    .close = ext2_vfs_close,
    .list = ext2_vfs_list,
    .readdir = ext2_vfs_readdir,
    .stat = ext2_vfs_stat,
    .statx = ext2_vfs_statx,
    .mkdir = ext2_vfs_mkdir,
    .unlink = ext2_vfs_unlink,
    .rename = NULL, /* a name of a file changes by a copy and a removal */
    .chmod = ext2_vfs_chmod,
    .chown = ext2_vfs_chown,
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

int ext2_vfs_truncate(const char *path, k_u64_t size)
{
    const char *relative;
    struct ext2_inode inode;
    int rc;

    if (path == NULL || !g_ext2.mounted) {
        return KERR_INVAL;
    }

    relative = ext2_relative(&g_ext2, path);
    if (relative == NULL) {
        return KERR_NOENT;
    }

    rc = volume_lock(&g_ext2);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_lookup(&g_ext2, relative, &inode, NULL);
    if (rc == 0) {
        rc = ext2_truncate_inode(&g_ext2, &inode, size);
    }

    k_mutex_unlock(&g_ext2.lock);

    return rc;
}

int ext2_check(const char *point, struct ext2_check_report *report)
{
    int rc;

    if (point == NULL || report == NULL) {
        return KERR_INVAL;
    }

    if (!g_ext2.mounted) {
        return KERR_NOENT;
    }

    if (strcmp(g_ext2.point, point) != 0) {
        return KERR_INVAL; /* not the volume of this filesystem */
    }

    rc = volume_lock(&g_ext2);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_check_volume(&g_ext2, report);

    k_mutex_unlock(&g_ext2.lock);

    return rc;
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
    case KERR_NOSPC:
        return "no space left on the volume";
    case KERR_NOTSUP:
        return "operation not supported by the volume";
    default:
        return "unknown error";
    }
}
