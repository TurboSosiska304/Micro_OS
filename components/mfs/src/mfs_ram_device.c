/*
 * Micro OS — a RAM backed volume for the tests of the filesystem.
 */
#include "mfs/mfs_ram_device.h"

#include <stdlib.h>
#include <string.h>

static int ram_read_block(void *ctx, k_u32_t block, void *buf)
{
    struct mfs_ram_device *dev = (struct mfs_ram_device *)ctx;

    if (block >= dev->blocks) {
        return -1;
    }

    memcpy(buf, dev->data + (size_t)block * MFS_BLOCK_SIZE, MFS_BLOCK_SIZE);
    dev->reads++;

    return 0;
}

static int ram_write_block(void *ctx, k_u32_t block, const void *buf)
{
    struct mfs_ram_device *dev = (struct mfs_ram_device *)ctx;

    if (block >= dev->blocks) {
        return -1;
    }

    memcpy(dev->data + (size_t)block * MFS_BLOCK_SIZE, buf, MFS_BLOCK_SIZE);
    dev->writes++;

    return 0;
}

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

void mfs_ram_device(struct mfs_ram_device *dev, struct mfs_device *out)
{
    out->blocks = dev->blocks;
    out->read_block = ram_read_block;
    out->write_block = ram_write_block;
    out->ctx = dev;
}

void mfs_ram_free(struct mfs_ram_device *dev)
{
    if (dev != NULL) {
        free(dev->data);
        dev->data = NULL;
    }
}
