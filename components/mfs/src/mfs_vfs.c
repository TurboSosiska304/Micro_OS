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
    {MFS_OK, KERR_NONE},        {MFS_ENOENT, KERR_NOENT},   {MFS_EEXIST, KERR_EXIST},
    {MFS_ENOSPC, KERR_NOSPC},   {MFS_EIO, KERR_IO},         {MFS_EINVAL, KERR_INVAL},
    {MFS_ENOTDIR, KERR_NOTDIR}, {MFS_EISDIR, KERR_ISDIR},   {MFS_ENOTEMPTY, KERR_NOTEMPTY},
    {MFS_E2BIG, KERR_TOOBIG},   {MFS_ENOTSUP, KERR_NOTSUP}, {MFS_ENOMEM, KERR_NOMEM},
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

/*
 * The private data of a descriptor of the VFS is a fixed buffer of 64 bytes
 * and the VFS hands it to the filesystem as it is. A file of MicroFS has to
 * fit there, cursor and all: the size of the structure is a contract with
 * kernel/vfs.h, and the check is here where the two meet.
 */
_Static_assert(sizeof(struct mfs_vfs_file) <= VFS_FILE_DATA_SIZE,
               "the file of MicroFS does not fit into a descriptor of the VFS");

/*
 * The buffer of one directory, for the calls that list it. It is not on the
 * stack: the entries of a directory of MicroFS do not fit there, and the
 * filesystem is one volume anyway.
 */
static struct mfs_dirent g_entries[MFS_MAX_ENTRIES];

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

static int vfs_read_page(void *ctx, k_u32_t inode, k_u64_t offset, void *buf, k_size_t size)
{
    int rc;

    K_UNUSED(ctx);

    rc = mfs_read_at(inode, offset, buf, size);
    return (rc < 0) ? mfs_to_kerr(rc) : rc;
}

static int vfs_write_page(void *ctx, k_u32_t inode, k_u64_t offset, const void *buf, k_size_t size)
{
    int rc;

    K_UNUSED(ctx);

    rc = mfs_write_at(inode, offset, buf, size);
    return (rc < 0) ? mfs_to_kerr(rc) : rc;
}

static int vfs_sync_file(void *ctx, k_u32_t inode)
{
    K_UNUSED(ctx);
    K_UNUSED(inode);

    return mfs_to_kerr(mfs_sync());
}

static int vfs_truncate_file(void *ctx, struct vfs_file *vfs_file, k_u64_t size)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;
    int rc;

    K_UNUSED(ctx);

    if (file == NULL) {
        return mfs_to_kerr(MFS_EINVAL);
    }

    rc = mfs_truncate(&file->file, size);
    if (rc == MFS_OK) {
        vfs_file->size = file->file.size;
    }

    return mfs_to_kerr(rc);
}

static int vfs_close_file(void *ctx, struct vfs_file *vfs_file)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;

    K_UNUSED(ctx);

    if (file == NULL) {
        return mfs_to_kerr(MFS_EINVAL);
    }

    if (vfs_file->ops != NULL && vfs_file->ops->readpage != NULL &&
        !file->file.dirty) {
        struct mfs_inode inode;

        file->file.offset = (k_u32_t)vfs_file->offset;
        file->file.size = (k_u32_t)vfs_file->size;
        if (mfs_inode_info(vfs_file->inode, &inode) == MFS_OK) {
            file->file.size = inode.size;
            file->file.first_block = inode.first_block;
            file->file.pending = (inode.flags & MFS_INODE_PENDING) != 0u;
            file->file.dirty = false;
        }
    } else {
        vfs_file->size = file->file.size;
        vfs_file->offset = file->file.offset;
    }

    return mfs_to_kerr(mfs_close(&file->file));
}

static int vfs_list_dir(void *ctx, const char *path, struct vfs_dirent *out, k_size_t max,
                        k_size_t *count)
{
    k_size_t found = 0;
    k_size_t i;
    int rc;

    K_UNUSED(ctx);

    /* The entries of the previous call must not be counted again. */
    memset(g_entries, 0, sizeof(g_entries));

    rc = mfs_list(path, g_entries, MFS_MAX_ENTRIES);
    if (rc != MFS_OK) {
        return mfs_to_kerr(rc);
    }

    for (i = 0; i < MFS_MAX_ENTRIES; i++) {
        if (g_entries[i].name_len == 0 && g_entries[i].inode == 0) {
            continue;
        }

        if (found >= max) {
            break;
        }

        memcpy(out[found].name, g_entries[i].name, sizeof(out[found].name));
        out[found].name[MFS_NAME_MAX] = '\0';
        out[found].inode = g_entries[i].inode;
        out[found].type = (g_entries[i].type & MFS_INODE_DIR) ? 2 : 1;
        out[found].size = g_entries[i].size;
        found++;
    }

    if (count != NULL) {
        *count = found;
    }

    return MFS_OK;
}

static int vfs_readdir_dir(void *ctx, struct vfs_file *vfs_file, struct vfs_dirent *out,
                           k_size_t max, k_size_t *count)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;
    k_size_t found = 0;
    k_size_t i;
    int rc;

    K_UNUSED(ctx);

    if (file == NULL || out == NULL || count == NULL) {
        return mfs_to_kerr(MFS_EINVAL);
    }

    *count = 0;

    if (max == 0) {
        return MFS_OK;
    }

    /* The buffer of the adapter is one directory of this volume. */
    if (max > MFS_MAX_ENTRIES) {
        max = MFS_MAX_ENTRIES;
    }

    memset(g_entries, 0, sizeof(g_entries));

    rc = mfs_readdir(&file->file, g_entries, max, &found);
    if (rc != MFS_OK) {
        return mfs_to_kerr(rc);
    }

    for (i = 0; i < found; i++) {
        memcpy(out[i].name, g_entries[i].name, sizeof(out[i].name));
        out[i].name[MFS_NAME_MAX] = '\0';
        out[i].inode = g_entries[i].inode;
        out[i].type = (g_entries[i].type & MFS_INODE_DIR) ? 2 : 1;
        out[i].size = g_entries[i].size;
    }

    *count = found;

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

    /*
     * The bits of the mode and of the owner live in the flags of the record,
     * and the adapter is the only place of the kernel that knows that: the
     * VFS and the system calls ask for the numbers, not for the packing.
     */
    out->mode = mfs_inode_mode(&inode);
    out->owner = mfs_inode_owner(&inode);
    /* The group of a version 3 record; a volume of version 1 or 2 has no
     * word for it and the reader answers zero, the group of root. */
    out->group = mfs_inode_group(&inode);

    return mfs_to_kerr(MFS_OK);
}

static int vfs_statx_file(void *ctx, const char *path, struct vfs_statx *out)
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

    out->inode = dirent.inode;
    out->type = (inode.flags & MFS_INODE_DIR) ? 2 : 1;
    out->size = inode.size;
    out->mode = mfs_inode_mode(&inode);
    out->owner = mfs_inode_owner(&inode);
    /*
     * The group is a word of its own in the record of a version 3 volume
     * (ADR 0038); a volume of version 1 or 2 has no group and the reader
     * answers zero, the group of root. The same number decides the triad of
     * the group in the check of the access (ADR 0044) and is the answer of
     * the stat call above.
     */
    out->group = mfs_inode_group(&inode);
    /*
     * The record of a version 1 volume has no room for the two moments,
     * and the reader of the filesystem answers zero: "unknown", not a
     * moment of 1970.
     */
    out->mtime = inode.mtime;
    out->ctime = inode.ctime;

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

static int vfs_remove_dir(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    return mfs_to_kerr(mfs_rmdir(path));
}

static int vfs_rename_object(void *ctx, const char *from, const char *to)
{
    K_UNUSED(ctx);
    return mfs_to_kerr(mfs_rename(from, to));
}

static int vfs_set_mode(void *ctx, const char *path, k_u32_t mode)
{
    K_UNUSED(ctx);
    return mfs_to_kerr(mfs_chmod(path, mode));
}

static int vfs_set_owner(void *ctx, const char *path, k_u32_t owner)
{
    K_UNUSED(ctx);
    return mfs_to_kerr(mfs_chown(path, owner));
}

static int vfs_set_group(void *ctx, const char *path, k_u32_t group)
{
    K_UNUSED(ctx);

    /*
     * A volume of MicroFS version 1 or 2 has no word of a group and answers
     * MFS_ENOTSUP, which mfs_to_kerr turns into KERR_NOTSUP: the attribute is
     * not there, the call is not forbidden. A version 3 volume keeps it.
     */
    return mfs_to_kerr(mfs_chgrp(path, group));
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
    .readpage = vfs_read_page,
    .writepage = vfs_write_page,
    .sync = vfs_sync_file,
    .truncate = vfs_truncate_file,
    .close = vfs_close_file,
    .list = vfs_list_dir,
    .readdir = vfs_readdir_dir,
    .stat = vfs_stat_file,
    .statx = vfs_statx_file,
    .mkdir = vfs_make_dir,
    .unlink = vfs_remove,
    .rmdir = vfs_remove_dir,
    .rename = vfs_rename_object,
    .chmod = vfs_set_mode,
    .chown = vfs_set_owner,
    .chgrp = vfs_set_group,
    .statfs = mfs_statfs_volume,
    .strerror = vfs_strerror_mfs,
};

int mfs_vfs_register(const char *point)
{
    return vfs_mount(point, &g_mfs_operations, NULL);
}
