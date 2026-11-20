/*
 * Micro OS — the inside of MicroFS.
 *
 * The public header of the filesystem is all a user of it needs; this one
 * is what the parts of the filesystem share between themselves. It is not
 * installed and nothing outside components/mfs includes it.
 *
 * The state below is handed to the checker of the volume by mfs_check(),
 * which holds the lock of the filesystem for the whole walk: the pointers
 * are the live state of the mounted volume and are only valid while that
 * lock is held.
 */
#ifndef MICRO_OS_MFS_SRC_MFS_PRIVATE_H
#define MICRO_OS_MFS_SRC_MFS_PRIVATE_H

#include "mfs/mfs.h"

/*
 * The map of the free blocks lives in the memory of the filesystem and is
 * written to the device in whole blocks. Two of them cover the biggest
 * volume that mounts, so the checker may size its own map the same way.
 */
#define MFS_BITMAP_MAX_BLOCKS 2

/* The end of a chain of the data blocks. */
#define MFS_NEXT_NONE 0xFFFFFFFFu

/* The bytes of data in a block: the first four hold the next block. */
#define MFS_BLOCK_DATA (MFS_BLOCK_SIZE - 4)

/* The view of the mounted volume that the checker works with. */
struct mfs_private {
    const struct mfs_superblock *sb;
    const k_u8_t *bitmap; /* the map of the free blocks, in the memory */
    k_u8_t *dir_scratch;  /* the buffer of a directory, owned by the filesystem */
    k_size_t dir_scratch_size;
    k_u32_t bitmap_blocks;  /* the blocks of the bitmap in the META section */
    k_u32_t inode_blocks;   /* the blocks of the table of the inodes */
    k_u32_t inode_capacity; /* the number of the records of the table */
    k_u32_t device_blocks;  /* the sectors of the device under the volume */
};

/*
 * Read one block of the mounted volume. The block has to be a block of the
 * volume; MFS_EIO says that the device could not deliver it.
 */
int mfs_priv_block_read(k_u32_t block, void *buf);

/* The checksum of the header of the volume, for the check of the header. */
k_u32_t mfs_priv_crc32(const void *data, k_size_t size);

/*
 * The check itself, called by mfs_check() with the lock of the filesystem
 * taken and the report already cleared.
 */
int mfs_check_locked(const struct mfs_private *view, struct mfs_check_report *report);

#endif /* MICRO_OS_MFS_SRC_MFS_PRIVATE_H */
