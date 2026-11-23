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

/* The most devices one system can have at the same time: the flash or the
 * disk of the port, its partitions, and the volumes of the tests and of the
 * simulator. */
#define K_BLOCKDEV_MAX 8

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

/*
 * The number of sectors the block layer keeps between the filesystem and a
 * device that asked for the cache. The cache is read through: a read looks
 * in the copy before it asks the medium. A write goes to the medium at once
 * or stays in the copy until the next sync, which is the choice of the
 * device (k_blockdev_register_cached).
 *
 * Sixteen sectors is the working set of a mount and of a directory walk:
 * the group descriptors and the bitmaps of ext2, the inode table of MicroFS,
 * the first cluster of a FAT - and, with the writes held back, the run of
 * sectors that a filesystem writes again for every block it allocates.
 *
 * It is the default of k_blockdev_register_cached() and not a ceiling: a
 * device whose medium moves a run at a time asks for a cache that holds the
 * run (k_blockdev_register_cached_sized), which is what the disk of the
 * x86-64 port does with half a megabyte of it.
 */
#define K_BLOCKDEV_CACHE_SECTORS 16

/* The largest cache a device may ask for: a million sectors of the smallest
 * unit would be half a gigabyte of RAM for the cache of one device, and a
 * port that asks for one is a port with a bug. */
#define K_BLOCKDEV_CACHE_MAX 65536

/*
 * The sectors one command of the medium brings when a read continues a run:
 * the read ahead of a file. Eight sectors are two blocks of the ext2 of the
 * volume of this machine (4 KiB each) and eight blocks of MicroFS, and they
 * leave room in the cache for the metadata of the volume, which is read
 * between two sectors of a file. Sixteen - the whole cache - was measured
 * and is worse: the run evicts the metadata that the next block of the file
 * needs, and the read of the volume doubles.
 *
 * The number is the default of a device that says nothing, and it was
 * measured on a cache of sixteen boxes. A medium whose cache is a run of half
 * a megabyte and whose command moves a run - the two disks of the x86-64 port,
 * which set struct k_blockdev::read_ahead - wants a run of the cache and not
 * eight sectors: the read of a file of 128 MiB of the volume of 5 GiB went
 * from 33.8 to 40.8 MiB/s when the disks asked for 128, and the read of a file
 * of 16 MiB out of the cache of the host from 134 to 286 MiB/s. A run bigger
 * than the cache of the device is not a run: the read ahead is clamped to the
 * boxes of the cache.
 */
#define K_BLOCKDEV_READ_AHEAD 8

struct k_blockdev {
    const char *name; /* "flash0", "sd0", ... */
    k_u32_t sector_size;
    k_u32_t sectors;
    const struct k_blockdev_ops *ops;
    void *ctx; /* the private data of the device, handed to every call */

    /*
     * The sectors a read ahead of a file asks of the medium (K_BLOCKDEV_READ_AHEAD
     * when the device says nothing). A device whose command moves a run - a
     * disk behind a bus master, a card - names a run of its own here, and the
     * block layer clamps it to the boxes of the cache of the device. It is a
     * hint and not a promise: the read ahead happens when a read continues the
     * stream of the last one.
     */
    k_u32_t read_ahead;
};

/*
 * Register a device. The structure and its context have to live as long as
 * the system: the platform owns them, the kernel keeps the pointer.
 * Returns 0, -1 when the table is full or the device is not usable.
 *
 * A device that is registered this way is asked for every sector its
 * filesystem wants, and the block layer keeps nothing.
 */
int k_blockdev_register(const struct k_blockdev *dev);

/*
 * Register a device with a cache of the last sectors the filesystem touched
 * (K_BLOCKDEV_CACHE_SECTORS), and the read ahead of a file (K_BLOCKDEV_READ_AHEAD).
 *
 * The platform asks for it when a sector of the medium is expensive - a
 * platter, a card, a flash chip behind a driver with a transaction of its
 * own - and leaves it off when the device is the memory of the machine,
 * which has a cache of its own and better.
 *
 * `write_back` is what the cache does with a write: with it, the sector
 * stays in the cache until the cache needs the room or the filesystem asks
 * for a sync, and a run of sectors leaves it as one command of the medium;
 * without it, every sector reaches the medium at once (write-through).
 *
 * A cached device is registered twice in a sense: the structure the platform
 * owns is not the pointer the table hands out, because the table needs a
 * place to attach the cache to. Everything the callers of the table see -
 * the name, the size, the operations - is the same.
 */
int k_blockdev_register_cached(const struct k_blockdev *dev, bool write_back);

/*
 * The same, with a cache of the size the device asks for: `sectors` boxes of
 * the sector of the device, at most K_BLOCKDEV_CACHE_MAX. A medium that moves
 * a run of 64 KiB in one command wants a cache that holds a run; a flash chip
 * of a board with a hundred kilobytes of RAM wants the sixteen of the
 * default. Zero is refused: a device with a cache of no boxes is a device
 * without a cache and is registered by k_blockdev_register().
 */
int k_blockdev_register_cached_sized(const struct k_blockdev *dev, bool write_back,
                                     k_size_t sectors);

/*
 * The state of the cache of a device, for a person who reads /proc. `dev` is
 * the device as the table hands it out or the structure the platform
 * registered; both answer the same. A device without a cache answers with
 * `present` false and the rest zero, and a device that is not in the table is
 * -1.
 */
struct k_blockdev_cache_stats {
    bool present;     /* the device has a cache at all */
    bool live;        /* the memory of the cache is allocated */
    bool write_back;  /* a write waits for the room or for a sync */
    k_u32_t capacity; /* the boxes of the cache */
    k_u32_t valid;    /* the boxes that hold a sector */
    k_u32_t dirty;    /* the boxes the medium does not have yet */
    k_u32_t sectors;  /* the sectors of the device */
    k_u32_t sector_size;
};

int k_blockdev_cache_stats(const struct k_blockdev *dev, struct k_blockdev_cache_stats *out);

/*
 * Take a device out of the table. The caller guarantees that nobody is using
 * it: the filesystem mounted on it is unmounted and no task holds a file of
 * it open. Returns 0, -1 when the device is not in the table.
 *
 * It exists for the devices that come and go - the partitions of a disk that
 * is scanned again, a card that is taken out - and for the tests, which
 * register a device of their own and have to leave the table as they found
 * it.
 */
int k_blockdev_unregister(const struct k_blockdev *dev);

/* The devices of the system, in the order they were registered. */
k_size_t k_blockdev_count(void);
const struct k_blockdev *k_blockdev_at(k_size_t index);

/* A device by name, NULL when there is none. */
const struct k_blockdev *k_blockdev_find(const char *name);

#endif /* MICRO_OS_KERNEL_BLOCKDEV_H */
