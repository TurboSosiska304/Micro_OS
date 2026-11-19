/*
 * Micro OS — mounting and formatting a volume of MicroFS.
 *
 * The filesystem is a consumer of blocks: it does not know whether the
 * volume lives on the flash of the board, on a card or in the memory of a
 * test. The device is looked up by name in the registry of the kernel, so
 * this file has no platform in it - the port registers "flash0" and the
 * filesystem mounts it.
 */
#include "mfs/mfs.h"
#include "mfs/mfs_vfs.h"

#include "kernel/blockdev.h"
#include "kernel/log.h"
#include "kernel/vfs.h"

/*
 * The number of the inodes of a new volume. A record takes 16 bytes, so
 * 256 of them fill four blocks of the table; the number is the one the
 * volumes of the board were made with, and a volume keeps the count it was
 * formatted with.
 */
#define MFS_MOUNT_RECORDS 256

/* A block of the medium as the flash comes in: all ones, never written. */
static bool block_is_blank(const struct k_blockdev *dev, k_u32_t index)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_size_t i;

    if (index >= dev->sectors) {
        return false;
    }

    if (dev->ops->read(dev->ctx, index, block, 1) != 0) {
        return false;
    }

    for (i = 0; i < MFS_BLOCK_SIZE; i++) {
        if (block[i] != 0xFF) {
            return false;
        }
    }

    return true;
}

/*
 * The medium was never written when the block of the header is all ones -
 * and so is the first block of the volume, where the root directory lives.
 * Both have to be blank: a volume that grew or moved has a blank header
 * block at its new end while its root is still there, and formatting that
 * would erase the data of the user without a word. Any other failure keeps
 * the data and leaves the board without a filesystem - that is better than
 * quietly erasing it.
 */
static bool volume_is_fresh(const struct k_blockdev *dev)
{
    return block_is_blank(dev, dev->sectors - 1u) && block_is_blank(dev, 0);
}

static int volume_format(const struct k_blockdev *dev)
{
    int rc = mfs_format(dev, MFS_MOUNT_RECORDS);

    if (rc != MFS_OK) {
        kerror("mfs: cannot format the volume of '%s' (rc=%d)", dev->name != NULL ? dev->name : "?",
               rc);
    }

    return rc;
}

static const struct k_blockdev *volume_find(const char *device)
{
    const struct k_blockdev *dev;

    if (device == NULL) {
        return NULL;
    }

    dev = k_blockdev_find(device);
    if (dev == NULL) {
        kerror("mfs: there is no block device '%s'", device);
    }

    return dev;
}

int mfs_vfs_format(const char *device)
{
    const struct k_blockdev *dev = volume_find(device);

    if (dev == NULL) {
        return MFS_EINVAL;
    }

    /*
     * The state of a mounted volume describes the sectors of the old
     * volume; it has to go before they are overwritten. The unmount writes
     * what is still in the memory of the filesystem out, which is the
     * state the format is about to replace anyway.
     */
    mfs_unmount();

    return volume_format(dev);
}

int mfs_vfs_mount(const char *point, const char *device)
{
    const struct k_blockdev *dev = volume_find(device);
    struct mfs_info info;
    int rc;

    if (dev == NULL) {
        return MFS_EINVAL;
    }

    rc = mfs_mount(dev);
    if (rc != MFS_OK) {
        if (!volume_is_fresh(dev)) {
            kerror("mfs: the volume of '%s' cannot be mounted (rc=%d), the data is kept", device,
                   rc);
            return rc;
        }

        kinfo("mfs: the volume of '%s' is not formatted yet, formatting it", device);
        rc = volume_format(dev);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = mfs_mount(dev);
        if (rc != MFS_OK) {
            kerror("mfs: cannot mount the volume of '%s' (rc=%d)", device, rc);
            return rc;
        }
    }

    if (mfs_get_info(&info) == MFS_OK) {
        kinfo("mfs: %u KB mounted, %u KB free, %u inodes", (unsigned)info.data_blocks,
              (unsigned)info.data_free, (unsigned)info.inodes_total);
    }

    return mfs_vfs_register(point);
}
