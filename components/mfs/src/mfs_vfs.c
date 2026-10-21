/*
 * Micro OS — the adapter between the filesystem and the VFS.
 *
 * The MicroFS is a singleton: one volume at a time. The adapter translates
 * the calls of the VFS into the calls of the filesystem.
 */
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"

#include <string.h>

#include "kernel/vfs.h"

/* The file of the VFS holds the file of the filesystem. */
struct mfs_vfs_file {
    struct mfs_file file;
};

static int vfs_open_file(void *ctx, const char *path, int flags, struct vfs_file *out,
                         k_size_t slot)
{
    struct mfs_vfs_file *file;
    int rc;

    K_UNUSED(ctx);
    K_UNUSED(slot);

    file = (struct mfs_vfs_file *)out->priv;
    if (file == NULL) {
        return MFS_EINVAL;
    }

    rc = mfs_open(path, flags, &file->file);
    if (rc != MFS_OK) {
        return rc;
    }

    out->inode = file->file.inode;
    out->size = file->file.size;
    out->offset = 0;

    return MFS_OK;
}

static int vfs_read_file(void *ctx, struct vfs_file *vfs_file, void *buf, k_size_t size)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;

    K_UNUSED(ctx);

    if (file == NULL) {
        return MFS_EINVAL;
    }

    vfs_file->offset = file->file.offset;

    return mfs_read(&file->file, buf, size);
}

static int vfs_write_file(void *ctx, struct vfs_file *vfs_file, const void *buf, k_size_t size)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;

    K_UNUSED(ctx);

    if (file == NULL) {
        return MFS_EINVAL;
    }

    vfs_file->offset = file->file.offset;

    return mfs_write(&file->file, buf, size);
}

static int vfs_close_file(void *ctx, struct vfs_file *vfs_file)
{
    struct mfs_vfs_file *file = (struct mfs_vfs_file *)vfs_file->priv;

    K_UNUSED(ctx);

    if (file == NULL) {
        return MFS_EINVAL;
    }

    vfs_file->size = file->file.size;

    return mfs_close(&file->file);
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
        return rc;
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
    int rc;

    K_UNUSED(ctx);

    rc = mfs_stat(path, &inode, NULL);
    if (rc != MFS_OK) {
        return rc;
    }

    out->inode = inode.flags; /* the caller gets the type from the flags */
    out->type = (inode.flags & MFS_INODE_DIR) ? 2 : 1;
    out->size = inode.size;

    return MFS_OK;
}

static int vfs_make_dir(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    return mfs_mkdir(path);
}

static int vfs_remove(void *ctx, const char *path)
{
    K_UNUSED(ctx);
    return mfs_unlink(path);
}

static int mfs_statfs_volume(void *ctx, struct vfs_statfs *out)
{
    struct mfs_info info;
    int rc;

    K_UNUSED(ctx);

    rc = mfs_get_info(&info);
    if (rc != MFS_OK) {
        return rc;
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
    .strerror = mfs_strerror,
};

/*
 * The VFS needs a place for the data of an open file. The kernel gives
 * every task a table of the descriptors, the private data of the file
 * lives here.
 */
static struct mfs_vfs_file g_files[16];

int mfs_vfs_register(const char *point)
{
    return vfs_mount(point, &g_mfs_operations, NULL);
}

void *mfs_vfs_file_data(k_size_t index)
{
    if (index >= 16) {
        return NULL;
    }

    return &g_files[index];
}
