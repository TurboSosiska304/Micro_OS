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

    /*
     * Flush the volatile cache of the device to the medium and wait until the
     * medium has the bytes. It is the guarantee of fsync: after this returns 0
     * a power cut keeps the data the device was told to write. NULL means the
     * device has no such guarantee to make - a volatile cache with no flush
     * command - and the caller answers KERR_NOTSUP rather than pretending.
     * A device that writes through or has no volatile cache answers 0.
     */
    int (*flush)(void *ctx);

    /*
     * The door of the writes (ADR 0041 §2). A medium whose bytes belong to
     * somebody else opens its writes only when a mount asks for them and
     * the rule of kernel/volume.c says yes; the driver keeps the door
     * closed until this call, and a driver without a door leaves the field
     * NULL. The call answers KERR_NOTSUP from the layers above a device
     * that has none, so a caller may ask every device the same question.
     *
     * A partition, a view and a cache forward the call to the medium below
     * them: the mounted device is not the one the controller registered.
     */
    int (*writes)(void *ctx, bool open);
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
 * A view of a device at a grain coarser than the medium's sector (ADR 0042).
 *
 * The sector of a device is a fact of the medium: a platter moves 512 bytes,
 * the flash of the board erases a page of 4096. The grain of a *volume* is a
 * fact of its format, and the two are not the same number: an ext2 volume of
 * 4096-byte blocks on a disk of 1 KiB sectors is one block per command of the
 * medium, and the block layer should hear one operation of 4 KiB and not four
 * of 1 KiB - the lock, the lookup of the cache and the copy are paid once.
 *
 * The view is that device: the sector of the volume over the sector of the
 * medium. It is not an entry of the table - the table is the machine's
 * devices and a volume is not one of them - so /proc and the shell list the
 * medium and not the grain a filesystem reads it at. The filesystem owns the
 * structure and opens it when its superblock names a grain the medium does
 * not have; the memory of the view is the caller's and nothing is allocated.
 *
 * `sector_size` has to be a multiple of the sector of the medium and larger
 * than it; the view covers the whole sectors of the medium it can name
 * (`medium->sectors / factor` of them) and the tail of the medium is not the
 * view's. A view of the grain of the medium is the medium itself and is
 * refused.
 */
struct k_blockdev_view {
    struct k_blockdev dev; /* the device a filesystem mounts; its name is the medium's */
    struct k_blockdev_ops ops;
    const struct k_blockdev *medium;
    k_u32_t factor; /* the sectors of the medium in one of the view */

    /*
     * What crossed the view: the operations of the block layer at the grain of
     * the volume and the sectors of the medium they became. The counters are
     * the measurement of the test that asks what a grain costs; no code
     * decides anything by them, and the close of the view forgets them.
     */
    k_u64_t reads;
    k_u64_t writes;
    k_u64_t sectors_read;
    k_u64_t sectors_written;
};

int k_blockdev_view_open(struct k_blockdev_view *view, const struct k_blockdev *medium,
                         k_u32_t sector_size);

/* Forget a view. Nothing is given back: the structure is the caller's. */
void k_blockdev_view_close(struct k_blockdev_view *view);

/* The device of the view, for a filesystem that mounts it. */
static inline const struct k_blockdev *k_blockdev_view_device(const struct k_blockdev_view *view)
{
    return &view->dev;
}

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

/*
 * Bring every device of the table to the medium: the cache of a device is
 * written out and the `sync` operation below it is asked - the FLUSH CACHE
 * EXT of a disk, the Flush of an NVMe namespace, the sync of a flash chip.
 *
 * It is the flush of the clean end (kernel/power.c, ADR 0050), and it is
 * the one call of the block layer a caller may make without a mount: a
 * device that nobody mounted still has a cache that may hold a sector.
 * Returns 0, or KERR_IO when one of the devices did not answer; the ones
 * after it are still asked, because a disk that failed is no reason to
 * leave the sectors of another in a cache.
 */
int k_blockdev_sync_all(void);

/*
 * Bring one device to the medium with the guarantee of an fsync: the cache of
 * the device is written out and the flush operation of the driver is asked.
 * Returns 0 on success, KERR_NOTSUP when the medium cannot make the promise,
 * or KERR_IO when the flush was attempted and failed.
 */
int k_blockdev_flush(const struct k_blockdev *dev);

/* A device by name, NULL when there is none. */
const struct k_blockdev *k_blockdev_find(const char *name);

/*
 * The door of the writes of a medium (ADR 0041 §2): whether the device has
 * one, and the call that opens or closes it.
 *
 * A device the platform registered without a `writes` operation is a medium
 * with no rule to ask - the memory of the machine, the flash of the board,
 * the image of the tests - and k_blockdev_writes() answers KERR_NOTSUP for
 * it: the caller may mount it for writing without a decision. A controller
 * that found a disk of a person puts the operation there, and a mount that
 * was not asked for the write leaves it closed.
 */
bool k_blockdev_gated(const struct k_blockdev *dev);
int k_blockdev_writes(const struct k_blockdev *dev, bool open);

#endif /* MICRO_OS_KERNEL_BLOCKDEV_H */
