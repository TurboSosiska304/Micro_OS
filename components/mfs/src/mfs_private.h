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
    k_u32_t inode_size;     /* 16 for a version 1 volume, 32 for version 2 */
    k_u32_t device_blocks;  /* the sectors of the device under the volume */
};

/*
 * The record of a version 1 volume. It has the four words of the record of
 * today and no time, which is why the two formats need a version of their
 * own (ADR 0013). The decoder below turns it into the record the rest of
 * the filesystem reads.
 */
struct mfs_inode_legacy {
    k_u32_t flags;
    k_u32_t first_block;
    k_u32_t size;
    k_u32_t entries;
};

_Static_assert(sizeof(struct mfs_inode_legacy) == MFS_INODE_SIZE_LEGACY,
               "the record of a legacy volume changed");

/*
 * Read a record from the table and write one back. The size of the record
 * is the one of the mounted volume, so a legacy volume is read and written
 * as the 16 bytes that it has - the bytes after a record belong to the next
 * one and must not be touched.
 */
void mfs_priv_inode_decode(const k_u8_t *raw, k_u32_t inode_size, struct mfs_inode *out);
void mfs_priv_inode_encode(const struct mfs_inode *inode, k_u32_t inode_size, k_u8_t *raw);

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

/* --- the repair pass --- */

/*
 * The table of records the repair can mark: one bit per record says whether
 * a name reaches it, and this map is 4 KiB for 32768 records. The largest
 * volume of the design (16 MiB) has 20927 records, so the map covers every
 * volume the documentation names; a table of 32768 records or more is
 * reported and left alone instead of being walked with a map that would hide
 * an orphan.
 */
#define MFS_REPAIR_INODES_MAX 32768u

/*
 * The repair pass is the only writer of the check of the volume, and it
 * writes through the filesystem itself: the map of the free blocks in the
 * memory and the device have to stay one state, so the pass never patches
 * the medium behind the back of the mount. Called by mfs_repair() with the
 * lock taken; report->fixed counts the changes it applied.
 */
int mfs_repair_locked(const struct mfs_private *view, struct mfs_check_report *report);

/*
 * The pieces of the filesystem that the repair pass uses. They are the
 * calls mfs.c already makes for a write of a user, so a repair is a write
 * like any other and the volume is the same one the next operation sees.
 */
void mfs_priv_bitmap_set(k_u32_t index, bool used);
int mfs_priv_inode_write(k_u32_t index, const struct mfs_inode *inode);
int mfs_priv_chain_set_next(k_u32_t block, k_u32_t next);
int mfs_priv_block_alloc(k_u32_t *block);
int mfs_priv_dir_write(const struct mfs_inode *inode, const k_u8_t *data, k_size_t size);
int mfs_priv_sync(void);

#endif /* MICRO_OS_MFS_SRC_MFS_PRIVATE_H */
