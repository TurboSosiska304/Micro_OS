/*
 * Micro OS — a RAM backed volume for the tests of the filesystem.
 *
 * The device is a plain array in the memory, but it is asked for its
 * sectors exactly the way the flash is: the tests then exercise the same
 * path through the block layer as the board does.
 */
#include "mfs/mfs_ram_device.h"

#include <stdlib.h>
#include <string.h>

static int ram_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct mfs_ram_device *dev = (struct mfs_ram_device *)ctx;
    size_t size = (size_t)count * MFS_BLOCK_SIZE;

    if (sector >= dev->blocks || count > dev->blocks - sector) {
        return -1;
    }

    memcpy(buf, dev->data + (size_t)sector * MFS_BLOCK_SIZE, size);
    dev->reads += (k_u32_t)count;

    return 0;
}

static int ram_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct mfs_ram_device *dev = (struct mfs_ram_device *)ctx;
    size_t size = (size_t)count * MFS_BLOCK_SIZE;

    if (sector >= dev->blocks || count > dev->blocks - sector) {
        return -1;
    }

    memcpy(dev->data + (size_t)sector * MFS_BLOCK_SIZE, buf, size);
    dev->writes += (k_u32_t)count;

    return 0;
}

/*
 * The volume is a stand-in for the flash, so an erase leaves the bits set.
 * The filesystem writes whole blocks and never asks for this, but a device
 * that cannot erase would be a worse model of the medium.
 */
static int ram_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    struct mfs_ram_device *dev = (struct mfs_ram_device *)ctx;
    size_t size = (size_t)count * MFS_BLOCK_SIZE;

    if (sector >= dev->blocks || count > dev->blocks - sector) {
        return -1;
    }

    memset(dev->data + (size_t)sector * MFS_BLOCK_SIZE, 0xFF, size);
    dev->writes += (k_u32_t)count;

    return 0;
}

static const struct k_blockdev_ops g_ram_ops = {
    .read = ram_read,
    .write = ram_write,
    .erase = ram_erase,
    .sync = NULL, /* the memory is written through */
};

int mfs_ram_init(struct mfs_ram_device *dev, k_u32_t blocks)
{
    if (dev == NULL || blocks == 0) {
        return -1;
    }

    dev->data = (k_u8_t *)calloc(blocks, MFS_BLOCK_SIZE);
    if (dev->data == NULL) {
        return -1;
    }

    dev->blocks = blocks;
    dev->writes = 0;
    dev->reads = 0;

    return 0;
}

void mfs_ram_device(struct mfs_ram_device *dev, struct k_blockdev *out)
{
    out->name = "ram0";
    out->sector_size = MFS_BLOCK_SIZE;
    out->sectors = dev->blocks;
    out->ops = &g_ram_ops;
    out->ctx = dev;
}

void mfs_ram_free(struct mfs_ram_device *dev)
{
    if (dev != NULL) {
        free(dev->data);
        dev->data = NULL;
    }
}
