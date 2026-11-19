/*
 * Micro OS — the block devices of the system.
 *
 * A filesystem is a consumer of blocks, not of a flash chip: MicroFS asks
 * for sector 17 and does not care whether it is a partition of the flash,
 * a card in a slot or an array in the memory of a test. The platform
 * registers the devices it has at the boot, and the filesystem mounts the
 * one it is told to mount.
 *
 * The shape follows the disk layer of Zephyr (struct disk_info: a sector
 * size, a count and read/write), which in turn is the block layer of
 * Linux reduced to what a filesystem needs: there is no request queue
 * here, because a board with 100 KB of SRAM has no queue to fill.
 */
#ifndef MICRO_OS_KERNEL_BLOCKDEV_H
#define MICRO_OS_KERNEL_BLOCKDEV_H

#include "kernel/types.h"

/* The most devices one system can have at the same time. */
#define K_BLOCKDEV_MAX 4

struct k_blockdev;

/*
 * The operations of a device. A sector is the unit of the device, not of
 * the filesystem: MicroFS asks for one sector at a time, the flash can
 * write a few of them at once.
 */
struct k_blockdev_ops {
    /* Read `count` sectors starting at `sector` into `buf`. 0 on success. */
    int (*read)(void *ctx, k_u32_t sector, void *buf, k_size_t count);

    /* Write `count` sectors starting at `sector` from `buf`. 0 on success. */
    int (*write)(void *ctx, k_u32_t sector, const void *buf, k_size_t count);

    /* Erase `count` sectors; NULL when the device needs no erase. */
    int (*erase)(void *ctx, k_u32_t sector, k_size_t count);

    /* Bring the writes to the medium; NULL when the device writes through. */
    int (*sync)(void *ctx);
};

struct k_blockdev {
    const char *name; /* "flash0", "sd0", ... */
    k_u32_t sector_size;
    k_u32_t sectors;
    const struct k_blockdev_ops *ops;
    void *ctx; /* the private data of the device, handed to every call */
};

/*
 * Register a device. The structure and its context have to live as long as
 * the system: the platform owns them, the kernel keeps the pointer.
 * Returns 0, -1 when the table is full or the device is not usable.
 */
int k_blockdev_register(const struct k_blockdev *dev);

/* The devices of the system, in the order they were registered. */
k_size_t k_blockdev_count(void);
const struct k_blockdev *k_blockdev_at(k_size_t index);

/* A device by name, NULL when there is none. */
const struct k_blockdev *k_blockdev_find(const char *name);

#endif /* MICRO_OS_KERNEL_BLOCKDEV_H */
