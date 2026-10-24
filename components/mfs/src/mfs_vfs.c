/*
 * Micro OS — the adapter between the filesystem and the VFS.
 *
 * The MicroFS is a singleton: one volume at a time. The adapter translates
 * the calls of the VFS into the calls of the filesystem.
 */
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/vfs.h"

/*
 * The filesystem has its own numbers for its errors and the kernel has
 * another set, and the two are close but not the same: the "directory is
 * not empty" of the filesystem is -9, and -9 of the kernel is a "bad file
 * descriptor". A program that gets the wrong number looks for the wrong
 * thing, so the adapter is the place where one is translated into the
 * other - the adapter is where the two contracts meet.
 */
struct mfs_code_pair {
    int mfs;
    int kerr;
};

static const struct mfs_code_pair g_codes[] = {
    {MFS_OK, KERR_NONE},        {MFS_ENOENT, KERR_NOENT}, {MFS_EEXIST, KERR_EXIST},
    {MFS_ENOSPC, KERR_NOSPC},   {MFS_EIO, KERR_IO},       {MFS_EINVAL, KERR_INVAL},
    {MFS_ENOTDIR, KERR_NOTDIR}, {MFS_EISDIR, KERR_ISDIR}, {MFS_ENOTEMPTY, KERR_NOTEMPTY},
    {MFS_E2BIG, KERR_TOOBIG},
};

/* The number of the kernel for a result of the filesystem. */
static int mfs_to_kerr(int rc)
{
    size_t i;

    for (i = 0; i < sizeof(g_codes) / sizeof(g_codes[0]); i++) {
        if (g_codes[i].mfs == rc) {
            return g_codes[i].kerr;
        }
    }

    /* An error that is not on the list is still an error of the disk. */
    return (rc < 0) ? KERR_IO : rc;
}

/* The number of the filesystem for a result of the kernel. */
static int kerr_to_mfs(int rc)
{
    size_t i;

    for (i = 0; i < sizeof(g_codes) / sizeof(g_codes[0]); i++) {
        if (g_codes[i].kerr == rc) {
            return g_codes[i].mfs;
        }
    }

    return MFS_EIO;
}

/* The text of an error of the filesystem, asked for with a number of the kernel. */
static const char *vfs_strerror_mfs(int error)
{
    return mfs_strerror(kerr_to_mfs(error));
}

/* The file of the VFS holds the file of the filesystem. */
struct mfs_vfs_file {
    struct mfs_file file;
};

static int vfs_open_file(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    struct mfs_vfs_file *file;
    int rc;

    K_UNUSED(ctx);

    file = (struct mfs_vfs_file *)out->priv;
    if (file == NULL) {
        return mfs_to_kerr(MFS_EINVAL);
    }

    rc = mfs_open(path, flags, &file->file);
    if (rc != MFS_OK) {
        return mfs_to_kerr(rc);
    }

    out->inode = file->file.inode;
    out->size = file->file.size;
    out->offset = 0;

    return MFS_OK;
}

static int vfs_read_file(void *ctx, struct vfs_file *vfs_file, void *buf, k_size_t size)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;
    int rc;

    K_UNUSED(ctx);

    if (file == NULL) {
        return mfs_to_kerr(MFS_EINVAL);
    }

    /*
     * The position of an open file belongs to the descriptor: a seek moves
     * it, and the filesystem has to read from the new place.
     */
    file->file.offset = (k_u32_t)vfs_file->offset;

    rc = mfs_read(&file->file, buf, size);
    if (rc > 0) {
        vfs_file->offset = file->file.offset;
        vfs_file->size = file->file.size;
    }

    /* A number of bytes is not an error and needs no translation. */
    return (rc < 0) ? mfs_to_kerr(rc) : rc;
}

static int vfs_write_file(void *ctx, struct vfs_file *vfs_file, const void *buf, k_size_t size)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;
    int rc;

    K_UNUSED(ctx);

    if (file == NULL) {
        return mfs_to_kerr(MFS_EINVAL);
    }

    /* The same as above: the descriptor decides where the data goes. */
    file->file.offset = (k_u32_t)vfs_file->offset;

    rc = mfs_write(&file->file, buf, size);
    if (rc > 0) {
        vfs_file->offset = file->file.offset;
        vfs_file->size = file->file.size;
    }

    return (rc < 0) ? mfs_to_kerr(rc) : rc;
}

static int vfs_close_file(void *ctx, struct vfs_file *vfs_file)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;

    K_UNUSED(ctx);

    if (file == NULL) {
        return mfs_to_kerr(MFS_EINVAL);
    }

    vfs_file->size = file->file.size;
    vfs_file->offset = file->file.offset;

    return mfs_to_kerr(mfs_close(&file->file));
}

static int vfs_list_dir(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                        k_size_t *count)
{
    static struct mfs_dirent entries[MFS_MAX_ENTRIES];
    k_size_t found = 0;
    k_size_t i;
    int rc;

    K_UNUSED(ctx);

    /* The entries of the previous call must not be counted again. */
    memset(entries, 0, sizeof(entries));

    rc = mfs_list(path, entries, MFS_MAX_ENTRIES);
    if (rc != MFS_OK) {
        return mfs_to_kerr(rc);
    }

    for (i = 0; i < MFS_MAX_ENTRIES; i++) {
        if (entries[i].name_len == 0 && entries[i].inode == 0) {
            continue;
        }

        if (found >= max) {
            break;
        }

        memcpy(out[found].name, entries[i].name, sizeof(out[found].name));
        out[found].name[MFS_NAME_MAX] = '\0';
        out[found].inode = entries[i].inode;
        out[found].type = (entries[i].type & MFS_INODE_DIR) ? 2 : 1;
        out[found].size = entries[i].size;
        found++;
    }

    if (count != NULL) {
        *count = found;
    }

    return MFS_OK;
}

static int vfs_stat_file(void *ctx, const char *path, struct vfs_stat *out)
{
    struct mfs_inode inode;
    struct mfs_dirent dirent;
    int rc;

    K_UNUSED(ctx);

    memset(&dirent, 0, sizeof(dirent));

    rc = mfs_stat(path, &inode, &dirent);
    if (rc != MFS_OK) {
        return mfs_to_kerr(rc);
    }

    /* The caller gets the number of the inode, not the flags of the file. */
    out->inode = dirent.inode;
    out->type = (inode.flags & MFS_INODE_DIR) ? 2 : 1;
    out->size = inode.size;

    return mfs_to_kerr(MFS_OK);
}

static int vfs_make_dir(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    return mfs_to_kerr(mfs_mkdir(path));
}

static int vfs_remove(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    return mfs_to_kerr(mfs_unlink(path));
}

static int mfs_statfs_volume(void *ctx, struct vfs_statfs *out)
{
    struct mfs_info info;
    int rc;

    K_UNUSED(ctx);

    rc = mfs_get_info(&info);
    if (rc != MFS_OK) {
        return mfs_to_kerr(rc);
    }

    out->total_blocks = info.data_blocks;
    out->free_blocks = info.data_free;
    out->total_inodes = info.inodes_total;
    out->free_inodes = info.inodes_free;
    out->block_size = info.block_size;

    return MFS_OK;
}

static const struct vfs_operations g_mfs_operations = {
    .open = vfs_open_file,
    .read = vfs_read_file,
    .write = vfs_write_file,
    .close = vfs_close_file,
    .list = vfs_list_dir,
    .stat = vfs_stat_file,
    .mkdir = vfs_make_dir,
    .unlink = vfs_remove,
    .statfs = mfs_statfs_volume,
    .strerror = vfs_strerror_mfs,
};

int mfs_vfs_register(const char *point)
{
    return vfs_mount(point, &g_mfs_operations, NULL);
}
