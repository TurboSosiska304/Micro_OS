/*
 * Micro OS — a RAM backed volume for the tests of the filesystem.
 */
#ifndef MICRO_OS_MFS_RAM_DEVICE_H
#define MICRO_OS_MFS_RAM_DEVICE_H

#include "mfs/mfs.h"

/* A volume of the given size in the memory. */
struct mfs_ram_device {
    k_u8_t *data;
    k_u32_t blocks;
    k_u32_t writes; /* the number of the written blocks */
    k_u32_t reads;  /* the number of the read blocks */
};

/* Prepare a volume of "blocks" blocks. */
int mfs_ram_init(struct mfs_ram_device *dev, k_u32_t blocks);

/* The device that the filesystem talks to. */
void mfs_ram_device(struct mfs_ram_device *dev, struct mfs_device *out);

void mfs_ram_free(struct mfs_ram_device *dev);

#endif /* MICRO_OS_MFS_RAM_DEVICE_H */
