#include "mfs/mfs.h"

#include <string.h>

#include "kernel/clock.h"
#include "kernel/kmm.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "mfs_private.h"

/*
 * The state of the mounted volume. The filesystem is a singleton: one
 * volume at a time, like the flash of the board.
 */
static struct {
    bool mounted;
    const struct k_blockdev *dev;
    struct mfs_superblock sb;
    k_u32_t inode_size;        /* 16 for a version 1 volume, 32 for 2, 64 for 3 */
    k_u32_t bitmap_blocks;     /* the blocks of the bitmap of the free blocks */
    k_u32_t bitmap_start;      /* the first block of the bitmap in META */
    k_u32_t inode_blocks;      /* the blocks of the table of the inodes */
    k_u32_t inode_capacity;    /* the number of the records of the table */
    k_u32_t table_end;         /* a version 3 volume: the block after the table */
    k_u32_t generation;        /* the number of the growths of the table */
    k_u32_t open_files;        /* the descriptors the filesystem handed out */
    bool bitmap_dirty;         /* the map in the memory is newer than the device */
    k_u8_t *bitmap;            /* the map of the free blocks, one bit per block */
    k_u32_t inode_cache_block; /* the block of the table that is cached */
    bool inode_cache_valid;    /* the cache holds the block of the medium */
    k_u8_t inode_cache[MFS_BLOCK_SIZE];
} g;

/*
 * Every data block starts with the number of the next block of the chain.
 * The end of the chain is marked with MFS_NEXT_NONE, so that a file can grow
 * without the blocks being continuous.
 */
/*
 * The lock of the filesystem.
 *
 * The volume is one piece of state: the map of the free blocks, the table
 * of the inodes and the cache of the sector of the header all describe the
 * same storage. Two tasks that work with it at the same time would give
 * the same block to two files, or write half of the state of the other.
 *
 * The mutex of the kernel is recursive because the helpers of mfs call
 * each other: a public call takes the lock and the function it delegates
 * to takes it again.
 */
static k_mutex_t s_lock = K_MUTEX_INITIALIZER;

static void mfs_lock(void)
{
    if (k_mutex_lock(&s_lock) != 0) {
        /* Out of memory for the lock is not a condition the filesystem can
         * work around: half of a volume is written by now. */
        KERNEL_PANIC("mfs: cannot take the lock of the filesystem");
    }
}

static void mfs_unlock(void)
{
    (void)k_mutex_unlock(&s_lock);
}

/* The blocks of one erase sector of the flash (4 KB). */
#define MFS_SECTOR_BLOCKS 4

/*
 * The checksum of the header of the volume. A small CRC32 (the polynomial
 * of IEEE 802.3), enough to notice a header that a brownout left half
 * written.
 */
static k_u32_t crc32(const void *data, k_size_t size)
{
    const k_u8_t *bytes = (const k_u8_t *)data;
    k_u32_t crc = 0xFFFFFFFFu;
    k_size_t i;
    int bit;

    for (i = 0; i < size; i++) {
        crc ^= bytes[i];

        for (bit = 0; bit < 8; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

/*
 * The header of the volume is checked again when the volume is checked,
 * and the checksum is the same one: the checker does not carry a second
 * copy of the polynomial.
 */
k_u32_t mfs_priv_crc32(const void *data, k_size_t size)
{
    return crc32(data, size);
}

/*
 * The checksum of a header of a version 3 volume covers every field of it
 * but the checksum itself: the map of the blocks, the table and the count of
 * the records decide where the volume is, and a header that only the first
 * half of it protects would mount a table that is not there. The header of a
 * version 1 and 2 volume was written by a system whose header ended after
 * forty-eight bytes, and the checksum of that one still covers exactly
 * those bytes, so an old volume keeps the checksum it was made with
 * (ADR 0038).
 */
k_u32_t mfs_priv_header_crc(const struct mfs_superblock *sb)
{
    struct mfs_superblock copy;

    if (sb->version < MFS_VERSION) {
        return crc32(sb, MFS_HEADER_CRC_SPAN);
    }

    /* The checksum is computed over the header with the field empty, so
     * that a reader reproduces it without a second copy of the polynomial
     * over a range that skips a hole. */
    copy = *sb;
    copy.crc = 0;

    return crc32(&copy, sizeof(copy));
}

/* --- the moments of a record --- */

/*
 * The time of a stamp comes from the clock of the kernel, never from a
 * library of the caller: the board keeps it in the RTC and the host in its
 * own process, and the filesystem must not know the difference (ADR 0013).
 * A machine whose clock was never set still has the seconds since the boot,
 * which order the changes even when they are not a date.
 *
 * A caller that builds an image may bring a source of its own
 * (mfs_set_time_source): one fixed moment makes the bytes of the volume
 * the same on every run, which a build of a disk needs and a running
 * machine does not.
 */
static mfs_now_fn s_now;

void mfs_set_time_source(mfs_now_fn now)
{
    mfs_lock();
    s_now = now;
    mfs_unlock();
}

static k_u64_t now_seconds(void)
{
    return s_now != NULL ? s_now() : k_clock_now_unix();
}

static void inode_stamp(struct mfs_inode *inode)
{
    k_u64_t now = now_seconds();

    inode->mtime = now;
    inode->ctime = now;
}

/* A change of the record alone: the content keeps the moment it had. */
static void inode_stamp_record(struct mfs_inode *inode)
{
    inode->ctime = now_seconds();
}

/*
 * The three formats of a record. The functions are the only places that
 * know the difference between the 16 bytes of a version 1 volume, the 32
 * bytes of version 2 and the 64 bytes with a checksum of version 3: the rest
 * of the filesystem works with struct mfs_inode and never writes past the
 * record of the volume.
 */
void mfs_priv_inode_decode(const k_u8_t *raw, k_u32_t inode_size, struct mfs_inode *out)
{
    memset(out, 0, sizeof(*out));

    if (inode_size == MFS_INODE_SIZE) {
        /* The first forty bytes of the record are the record of the memory:
         * the checksum and the padding after them are not part of it. */
        memcpy(out, raw, sizeof(*out));
        return;
    }

    if (inode_size == MFS_INODE_SIZE_TIME) {
        /* The same prefix without the group: a version 2 record has no
         * word of its own for it (ADR 0038). */
        memcpy(out, raw, MFS_INODE_SIZE_TIME);
        out->group = 0;
        return;
    }

    {
        struct mfs_inode_legacy legacy;

        memcpy(&legacy, raw, sizeof(legacy));
        out->flags = legacy.flags;
        out->first_block = legacy.first_block;
        out->size = legacy.size;
        out->entries = legacy.entries;
        /* No room for a time or a group: zero says "unknown", and it is
         * not a date or a group of root. */
        out->mtime = 0;
        out->ctime = 0;
        out->group = 0;
    }
}

void mfs_priv_inode_encode(const struct mfs_inode *inode, k_u32_t inode_size, k_u8_t *raw)
{
    memset(raw, 0, inode_size);

    if (inode_size == MFS_INODE_SIZE) {
        struct mfs_inode_record *record = (struct mfs_inode_record *)raw;

        memcpy(record, inode, sizeof(*inode));
        record->reserved = 0;
        memset(record->pad, 0, sizeof(record->pad));
        record->crc = mfs_record_crc(raw);
        return;
    }

    if (inode_size == MFS_INODE_SIZE_TIME) {
        memcpy(raw, inode, MFS_INODE_SIZE_TIME);
        return;
    }

    {
        struct mfs_inode_legacy legacy;

        legacy.flags = inode->flags;
        legacy.first_block = inode->first_block;
        legacy.size = inode->size;
        legacy.entries = inode->entries;
        memcpy(raw, &legacy, sizeof(legacy));
    }
}

/*
 * A record of version 1 and 2 has no checksum: the volume is trusted the way
 * it always was, and only the structural rules of the check speak about it.
 */
bool mfs_priv_inode_sound(const k_u8_t *raw, k_u32_t inode_size)
{
    k_u32_t stored;

    if (inode_size != MFS_INODE_SIZE) {
        return true;
    }

    memcpy(&stored, raw + MFS_RECORD_CRC_SPAN, sizeof(stored));

    return stored == mfs_record_crc(raw);
}

k_u32_t mfs_record_crc(const void *record)
{
    return crc32(record, MFS_RECORD_CRC_SPAN);
}

/* --- the low level access to the volume --- */

/*
 * The filesystem asks for one block at a time, and the block is one sector
 * of the device: the volume of MicroFS is laid out in sectors of the
 * medium, it has no notion of a request that spans several of them.
 */
static int dev_read(k_u32_t block, void *buf)
{
    if (g.dev == NULL || block >= g.sb.total_blocks) {
        return MFS_EIO;
    }

    if (g.dev->ops->read(g.dev->ctx, block, buf, 1) != 0) {
        return MFS_EIO;
    }

    return MFS_OK;
}

/* The checker reads the volume the same way the filesystem does. */
int mfs_priv_block_read(k_u32_t block, void *buf)
{
    return dev_read(block, buf);
}

static int dev_write(k_u32_t block, const void *buf)
{
    if (g.dev == NULL || block >= g.sb.total_blocks) {
        return MFS_EIO;
    }

    if (g.dev->ops->write(g.dev->ctx, block, buf, 1) != 0) {
        return MFS_EIO;
    }

    return MFS_OK;
}

/* The device hands the changes to the medium, when it holds them back. */
static int dev_sync(void)
{
    if (g.dev == NULL || g.dev->ops->sync == NULL) {
        return MFS_OK;
    }

    return (g.dev->ops->sync(g.dev->ctx) == 0) ? MFS_OK : MFS_EIO;
}

/* --- the bitmap of the free blocks --- */

static k_u32_t bitmap_bytes(void)
{
    return (g.sb.data_blocks + 7u) / 8u;
}

/* The blocks of the map that a volume of these many data blocks needs. */
static k_u32_t bitmap_blocks_for(k_u32_t data_blocks)
{
    k_u32_t bytes = (data_blocks + 7u) / 8u;

    return (bytes + MFS_BLOCK_SIZE - 1u) / MFS_BLOCK_SIZE;
}

/*
 * Give the map back. Every way out of a mount, a format or an unmount goes
 * through here, so that a volume leaves the allocator as it found it: the
 * map is the biggest thing the filesystem asks for, and a mount that fails
 * half way must not keep it.
 */
static void bitmap_release(void)
{
    if (g.bitmap != NULL) {
        kmm_free(g.bitmap);
        g.bitmap = NULL;
    }
}

/*
 * Allocate the map of the free blocks for a volume. The map is one bit per
 * data block, rounded up to whole blocks of the device, and comes from the
 * memory of the system: the filesystem cannot answer a call without it.
 *
 * A volume that asks for more than MFS_BITMAP_MAX_BYTES is refused here,
 * before a block is read, with MFS_EINVAL: the format and the mount both
 * speak about the map in the blocks of the device, and the number is the
 * size of the map, not of the volume.
 */
static int bitmap_alloc(k_u32_t blocks)
{
    k_u8_t *map;
    k_size_t size;

    if (blocks == 0u || blocks > MFS_BITMAP_MAX_BLOCKS) {
        kerror("mfs: a map of %u block(s) is beyond the %u the filesystem holds", (unsigned)blocks,
               (unsigned)MFS_BITMAP_MAX_BLOCKS);
        return MFS_EINVAL;
    }

    size = (k_size_t)blocks * MFS_BLOCK_SIZE;
    map = (k_u8_t *)kmm_alloc(size, KMM_PRIORITY_SYSTEM);

    if (map == NULL) {
        kerror("mfs: no memory for the map of the free blocks (%u block(s))", (unsigned)blocks);
        return MFS_ENOMEM;
    }

    memset(map, 0, size);

    /* A map of an earlier volume cannot describe this one. */
    bitmap_release();
    g.bitmap = map;

    return MFS_OK;
}

static int bitmap_load(void)
{
    k_u32_t blocks = bitmap_blocks_for(g.sb.data_blocks);
    k_u32_t i;
    int rc;

    /*
     * A version 3 volume stores the map at the top of META, next to INFO,
     * so that the table can grow downwards without moving it; an older
     * volume has it at the start of META and the table after it.
     */
    if (g.sb.version >= MFS_VERSION) {
        if (blocks != g.sb.bitmap_blocks || blocks > g.sb.meta_blocks) {
            return MFS_EINVAL;
        }
    }

    rc = bitmap_alloc(blocks);
    if (rc != MFS_OK) {
        return rc;
    }

    for (i = 0; i < blocks; i++) {
        rc = dev_read(g.bitmap_start + i, g.bitmap + i * MFS_BLOCK_SIZE);

        if (rc != MFS_OK) {
            /* The volume is not mounted, the map is not its state. */
            bitmap_release();
            return rc;
        }
    }

    g.bitmap_blocks = blocks;
    return MFS_OK;
}

static int bitmap_store(void)
{
    k_u32_t i;

    for (i = 0; i < g.bitmap_blocks; i++) {
        int rc = dev_write(g.bitmap_start + i, g.bitmap + i * MFS_BLOCK_SIZE);

        if (rc != MFS_OK) {
            return rc;
        }
    }

    return MFS_OK;
}

static bool bitmap_test(k_u32_t index)
{
    k_u32_t byte = index / 8u;
    k_u32_t bit = index % 8u;

    if (byte >= bitmap_bytes()) {
        return false;
    }

    return (g.bitmap[byte] & (1u << bit)) != 0;
}

static void bitmap_set(k_u32_t index, bool used)
{
    k_u32_t byte = index / 8u;
    k_u32_t bit = index % 8u;

    if (byte >= bitmap_bytes()) {
        return;
    }

    if (used) {
        g.bitmap[byte] |= (k_u8_t)(1u << bit);
    } else {
        g.bitmap[byte] &= (k_u8_t) ~(1u << bit);
    }

    /* The device has to learn about it, the next sync writes the map out. */
    g.bitmap_dirty = true;
}

/* Find a free block. */
static int block_alloc(k_u32_t *block)
{
    k_u32_t i;

    for (i = 0; i < g.sb.data_blocks; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i, true);
            *block = g.sb.data_start + i;
            return MFS_OK;
        }
    }

    return MFS_ENOSPC;
}

static void block_release(k_u32_t block)
{
    bitmap_set(block - g.sb.data_start, false);
}

/* --- the table of the inodes --- */

/* The records of one block of the table. */
static k_u32_t inode_records_per_block(void)
{
    return MFS_BLOCK_SIZE / g.inode_size;
}

/*
 * Where a record lies on the medium.
 *
 * A version 1 and 2 volume numbers its records from the start of the table
 * and the table fills META after the map. A version 3 volume numbers them
 * from the top of the table, which is anchored at the map next to INFO: the
 * table grows downwards into the data section, a record keeps its address
 * for the life of the volume, and the growth of the table is a write of new
 * records below the last one and a header that says so (ADR 0038).
 */
static void inode_address(k_u32_t version, k_u32_t meta_start, k_u32_t table_end,
                          k_u32_t bitmap_blocks, k_u32_t inode_size, k_u32_t index, k_u32_t *block,
                          k_u32_t *offset)
{
    if (version >= MFS_VERSION) {
        k_u32_t per_block = MFS_BLOCK_SIZE / inode_size;
        k_u32_t slots = index + 1u; /* the record 0 is the last slot of the top */

        *block = table_end - (slots + per_block - 1u) / per_block;
        *offset = ((per_block - slots % per_block) % per_block) * inode_size;
        return;
    }

    *block = meta_start + bitmap_blocks + (index * inode_size) / MFS_BLOCK_SIZE;
    *offset = (index * inode_size) % MFS_BLOCK_SIZE;
}

void mfs_priv_inode_address(const struct mfs_private *view, k_u32_t index, k_u32_t *block,
                            k_u32_t *offset)
{
    inode_address(view->sb->version, view->sb->meta_start, view->table_end, view->bitmap_blocks,
                  view->inode_size, index, block, offset);
}

/*
 * The block of the medium of the n-th block of the table in the order of the
 * walk, which is also the order of the records: the block holds the records
 * of [n * per_block, (n + 1) * per_block). A version 1 and 2 volume lays the
 * table out from its start, a version 3 volume from its top, so the walk of
 * the two goes in opposite directions over the medium.
 */
k_u32_t mfs_priv_table_block(const struct mfs_private *view, k_u32_t walk)
{
    if (view->sb->version >= MFS_VERSION) {
        return view->table_end - 1u - walk;
    }

    return view->sb->meta_start + view->bitmap_blocks + walk;
}

static k_u32_t inode_table_block(k_u32_t index)
{
    k_u32_t block;
    k_u32_t offset;

    inode_address(g.sb.version, g.sb.meta_start, g.table_end, g.bitmap_blocks, g.inode_size, index,
                  &block, &offset);

    return block;
}

static k_u32_t inode_table_offset(k_u32_t index)
{
    k_u32_t block;
    k_u32_t offset;

    inode_address(g.sb.version, g.sb.meta_start, g.table_end, g.bitmap_blocks, g.inode_size, index,
                  &block, &offset);

    return offset;
}

/*
 * The block of the table of the records, in the memory of the filesystem.
 *
 * A block holds 32 records of a version 2 volume, so a walk of the table -
 * the count of the used records, the search for a free one, the walk of the
 * check - used to read the same block once per record: the table of a 5 GiB
 * volume is 214 MiB and the walk cost 6.8 GiB of the medium. One block of
 * the cache makes the walk cost the table itself, which is what the format
 * says the table is.
 */
static int inode_table_load(k_u32_t block)
{
    int rc;

    if (g.inode_cache_valid && g.inode_cache_block == block) {
        return MFS_OK;
    }

    rc = dev_read(block, g.inode_cache);
    if (rc != MFS_OK) {
        g.inode_cache_valid = false;
        return rc;
    }

    g.inode_cache_block = block;
    g.inode_cache_valid = true;

    return MFS_OK;
}

static int inode_read_lenient(k_u32_t index, struct mfs_inode *out)
{
    k_u32_t offset;
    int rc;

    if (index == 0 || index > g.inode_capacity) {
        return MFS_EINVAL;
    }

    rc = inode_table_load(inode_table_block(index));
    if (rc != MFS_OK) {
        return rc;
    }

    offset = inode_table_offset(index);
    mfs_priv_inode_decode(g.inode_cache + offset, g.inode_size, out);

    return MFS_OK;
}

/*
 * Read a record. A used record of a version 3 volume carries its checksum,
 * and a record that does not match it is one that a power cut left half
 * written: the object it would have described was never committed, and the
 * call says that the volume cannot deliver it instead of handing out the
 * fields of a torn write. A free record is a record nobody claims and has
 * no checksum to keep. The check of the volume reads such a record the
 * lenient way and repairs it (ADR 0038).
 */
static int inode_read(k_u32_t index, struct mfs_inode *out)
{
    k_u32_t offset;
    int rc;

    if (index == 0 || index > g.inode_capacity) {
        return MFS_EINVAL;
    }

    rc = inode_table_load(inode_table_block(index));
    if (rc != MFS_OK) {
        return rc;
    }

    offset = inode_table_offset(index);
    mfs_priv_inode_decode(g.inode_cache + offset, g.inode_size, out);

    if ((out->flags & MFS_INODE_USED) != 0u &&
        !mfs_priv_inode_sound(g.inode_cache + offset, g.inode_size)) {
        kerror("mfs: the record %u of the table is not the one that was written", (unsigned)index);
        return MFS_EIO;
    }

    return MFS_OK;
}

static int inode_write(k_u32_t index, const struct mfs_inode *inode)
{
    k_u8_t raw[MFS_INODE_SIZE];
    k_u32_t block;
    k_u32_t offset;
    int rc;

    if (index == 0 || index > g.inode_capacity) {
        return MFS_EINVAL;
    }

    block = inode_table_block(index);

    rc = inode_table_load(block);
    if (rc != MFS_OK) {
        return rc;
    }

    offset = inode_table_offset(index);
    mfs_priv_inode_encode(inode, g.inode_size, raw);
    memcpy(g.inode_cache + offset, raw, g.inode_size);

    rc = dev_write(block, g.inode_cache);
    if (rc != MFS_OK) {
        /* The block of the medium is not the one of the cache any more. */
        g.inode_cache_valid = false;
        return rc;
    }

    return MFS_OK;
}

/* Find a free record of the table of the inodes. */
static int table_grow(k_u32_t add_blocks);

static int inode_alloc(k_u32_t *index)
{
    k_u32_t i;
    int attempt;

    for (attempt = 0; attempt < 2; attempt++) {
        for (i = 1; i <= g.inode_capacity; i++) {
            struct mfs_inode inode;
            int rc = inode_read_lenient(i, &inode);

            if (rc != MFS_OK) {
                return rc;
            }

            if ((inode.flags & MFS_INODE_USED) == 0) {
                *index = i;
                return MFS_OK;
            }
        }

        /*
         * A table with no free record grows into the data section: the
         * second walk sees the records the growth added, and the volume
         * keeps taking objects until its data section has nothing to give
         * (ADR 0038).
         */
        if (attempt != 0) {
            break;
        }

        {
            int rc = table_grow(0);

            if (rc != MFS_OK) {
                return rc;
            }
        }
    }

    return MFS_ENOSPC;
}

/* --- the mode and the owner --- */

/*
 * The mode and the owner live in the upper bits of the flags of a record,
 * see mfs.h and ADR 0005. These are the only two places that know the
 * shift: everything else asks for the numbers.
 */
k_u32_t mfs_inode_mode(const struct mfs_inode *inode)
{
    k_u32_t mode;

    if (inode == NULL) {
        return 0;
    }

    mode = (inode->flags >> MFS_MODE_SHIFT) & MFS_MODE_MASK;

    /*
     * The record that a volume of 0.3.x holds has no mode in it: the bits
     * were not there when the object was made. A zero is read as the mode
     * of the design for the type of the object, so an old volume keeps
     * working without being rewritten at the mount. The price is that
     * "chmod 000" is not expressible - mfs_chmod refuses the zero - and
     * that is a conscious choice while the board has one user.
     */
    if (mode == 0u) {
        return ((inode->flags & MFS_INODE_DIR) != 0u) ? MFS_MODE_DIR_DEFAULT : MFS_MODE_DEFAULT;
    }

    return mode;
}

k_u32_t mfs_inode_owner(const struct mfs_inode *inode)
{
    if (inode == NULL) {
        return 0;
    }

    return (inode->flags >> MFS_OWNER_SHIFT) & MFS_OWNER_MASK;
}

k_u32_t mfs_inode_group(const struct mfs_inode *inode)
{
    return (inode != NULL) ? inode->group : 0;
}

/* The flags of a new record: the type, the mode and the owner together. */
static k_u32_t inode_flags(k_u32_t type, k_u32_t mode, k_u32_t owner)
{
    return type | ((mode & MFS_MODE_MASK) << MFS_MODE_SHIFT) |
           ((owner & MFS_OWNER_MASK) << MFS_OWNER_SHIFT);
}

/* --- the chains of the blocks --- */

/* The number of the next block of a chain. */
static int chain_next(k_u32_t block, k_u32_t *next)
{
    k_u8_t buffer[MFS_BLOCK_SIZE];

    int rc = dev_read(block, buffer);

    if (rc != MFS_OK) {
        return rc;
    }

    memcpy(next, buffer, sizeof(*next));
    return MFS_OK;
}

static int chain_set_next(k_u32_t block, k_u32_t next)
{
    k_u8_t buffer[MFS_BLOCK_SIZE];
    int rc = dev_read(block, buffer);

    if (rc != MFS_OK) {
        return rc;
    }

    memcpy(buffer, &next, sizeof(next));
    return dev_write(block, buffer);
}

/*
 * The block of the chain that holds the given offset of the data.
 *
 * The chain is a list of blocks: a step to the next one is a read of the
 * header of the block, and a walk from the head is the only way to a block
 * that the descriptor has not seen yet. A read of a file a block at a time
 * walks the chain from the head for every block, which costs the square of
 * the blocks of the file - the start of a program of half a megabyte spends
 * seconds in the filesystem before the first instruction of it runs. The
 * descriptor remembers the block it used last, so a read after it walks
 * forward from there and a block costs one step; a seek back to an earlier
 * block starts at the head again, which is the price of going backwards.
 */
static int file_block_at(struct mfs_file *file, k_u32_t data_offset, k_u32_t *block,
                         k_u32_t *in_block)
{
    k_u32_t index = data_offset / MFS_BLOCK_DATA;
    k_u32_t current;
    k_u32_t i;

    if (file->chain_valid && index >= file->chain_index) {
        current = file->chain_block;
        i = file->chain_index;
    } else {
        current = file->first_block;
        i = 0;
    }

    for (; i < index; i++) {
        k_u32_t next;
        int rc = chain_next(current, &next);

        if (rc != MFS_OK) {
            return rc;
        }

        if (next == MFS_NEXT_NONE) {
            return MFS_EIO;
        }

        current = next;
    }

    file->chain_block = current;
    file->chain_index = index;
    file->chain_valid = true;

    *block = current;
    *in_block = data_offset % MFS_BLOCK_DATA;

    return MFS_OK;
}

/*
 * Forget the cursor: the next read of the descriptor starts at the head of
 * the chain. It is called where the chain the descriptor works with gets a
 * new head - the open of a file, and the first block of an empty file. A
 * block appended at the end of the chain does not need it: the blocks before
 * the tail are the ones they were, and the walk from the cursor reaches the
 * new block through the header it just got.
 */
static void file_cursor_forget(struct mfs_file *file)
{
    file->chain_valid = false;
    file->chain_block = MFS_NEXT_NONE;
    file->chain_index = 0;
    file->grow_valid = false;
    file->grow_block = MFS_NEXT_NONE;
    file->grow_index = 0;
}

/*
 * Bring a descriptor of an open file up to the volume after a growth of the
 * table of the records (ADR 0038). The growth moves the blocks of the chains
 * and patches the pointers of the chains, so the record of the object is the
 * truth about the head of the chain and the cursors of the descriptor are
 * not: the descriptor takes the head of its record again and walks from it.
 * The number of the blocks of the chain does not change, and neither does
 * the size the descriptor holds - a write that grew the chain past the
 * record has that number in the descriptor, not on the medium yet.
 *
 * A descriptor of a volume that did not grow pays one comparison.
 */
static int file_refresh(struct mfs_file *file)
{
    struct mfs_inode inode;
    int rc;

    if (file->generation == g.generation) {
        return MFS_OK;
    }

    rc = inode_read(file->inode, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    file->first_block = inode.first_block;
    file_cursor_forget(file);
    file->generation = g.generation;

    return MFS_OK;
}

/* The last block of a chain. */
static int chain_last(k_u32_t first, k_u32_t *last)
{
    k_u32_t current = first;
    k_u32_t guard = 0;

    for (;;) {
        k_u32_t next;
        int rc = chain_next(current, &next);

        if (rc != MFS_OK) {
            return rc;
        }

        if (next == MFS_NEXT_NONE) {
            *last = current;
            return MFS_OK;
        }

        current = next;

        if (++guard > g.sb.data_blocks) {
            return MFS_EIO; /* a loop in the chain */
        }
    }
}

/* The number of the blocks of a chain. */
static int chain_count(k_u32_t first, k_u32_t *count)
{
    k_u32_t current = first;
    k_u32_t guard = 0;
    k_u32_t n = 0;

    for (;;) {
        k_u32_t next;
        int rc = chain_next(current, &next);

        if (rc != MFS_OK) {
            return rc;
        }

        n++;

        if (next == MFS_NEXT_NONE) {
            *count = n;
            return MFS_OK;
        }

        current = next;

        if (++guard > g.sb.data_blocks) {
            return MFS_EIO;
        }
    }
}

/* Give the blocks of a chain back to the map of the free blocks. */
static int file_release_chain(k_u32_t first)
{
    k_u32_t count;
    k_u32_t block;
    k_u32_t i;

    if (first == MFS_NEXT_NONE) {
        return MFS_OK;
    }

    if (chain_count(first, &count) != MFS_OK) {
        return MFS_EIO;
    }

    block = first;

    for (i = 0; i < count; i++) {
        k_u32_t next = MFS_NEXT_NONE;

        chain_next(block, &next);
        block_release(block);

        if (next == MFS_NEXT_NONE) {
            break;
        }
        block = next;
    }

    return MFS_OK;
}

/* --- the growth of the table of the records --- */

/*
 * A free block of the data below a limit. block_alloc() is not it: a free
 * block of the tail is free and still about to be part of the table, so the
 * allocation of a block that is moved out of the tail has to look below the
 * new end of the data section.
 */
static int block_alloc_below(k_u32_t limit, k_u32_t *block)
{
    k_u32_t i;

    for (i = 0; i < limit; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i, true);
            *block = g.sb.data_start + i;
            return MFS_OK;
        }
    }

    return MFS_ENOSPC;
}

/* The block a block of the tail was moved to, or MFS_NEXT_NONE. */
static k_u32_t tail_new_block(const k_u32_t *from, const k_u32_t *to, k_u32_t count, k_u32_t block)
{
    k_u32_t i;

    for (i = 0; i < count; i++) {
        if (from[i] == block) {
            return to[i];
        }
    }

    return MFS_NEXT_NONE;
}

/*
 * The header of the volume, written back into its block of INFO. The block
 * is read first because only the last block of INFO holds the header and the
 * rest of the block is not the header's to erase.
 */
static int header_write(const struct mfs_superblock *sb)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t at = sb->info_start + sb->info_blocks - 1u;
    int rc;

    rc = dev_read(at, block);
    if (rc != MFS_OK) {
        return rc;
    }

    memcpy(block, sb, sizeof(*sb));

    return dev_write(at, block);
}

/*
 * Grow the table of the records of a version 3 volume by `add_blocks`
 * blocks, taking them from the top of the data section (ADR 0038).
 *
 * The blocks of the tail may hold the data of a file: each of them is moved
 * to a free block below the new end first, and the pointer of the chain that
 * names it is patched, so the data survives. The order is the promise of the
 * pass:
 *
 *   1. the replacement blocks are taken in the map and the map reaches the
 *      device, so a block the table is about to take is never free on the
 *      medium while a chain names it;
 *   2. every used block of the tail is copied and its chain pointer patched;
 *      a power cut between the two loses a block to the map (the check gives
 *      it back) and never the data;
 *   3. the tail blocks are written as empty records;
 *   4. the header is written last: it is the commit of the growth, and a
 *      volume that loses the power before it is the volume it was, with a
 *      few blocks moved and nothing lost.
 *
 * The table grows downwards from the map, so the records it has keep their
 * addresses and only the new ones are below them.
 */
static int table_grow(k_u32_t add_blocks)
{
    struct mfs_superblock sb;
    k_u32_t old_data;
    k_u32_t new_data;
    k_u32_t moves = 0;
    k_u32_t from[MFS_TABLE_GROW_BLOCKS * 8u];
    k_u32_t to[MFS_TABLE_GROW_BLOCKS * 8u];
    k_u32_t index;
    k_u32_t i;
    int rc;

    if (!g.mounted || g.sb.version < MFS_VERSION) {
        return MFS_ENOSPC;
    }

    /*
     * The blocks of the chains are moved by the growth, and the chain of an
     * open file is only half on the medium: the first block of a file that
     * was written but not closed is named by the descriptor and by no
     * record, so the growth would take it for a block nobody owns. A
     * descriptor that is open keeps the table where it is, and the create
     * that needed the records answers "no space" until it is closed.
     */
    if (g.open_files != 0u) {
        kerror("mfs: the table of the records cannot grow while %u file(s) are open",
               (unsigned)g.open_files);
        return MFS_ENOSPC;
    }

    if (add_blocks == 0u) {
        add_blocks = MFS_TABLE_GROW_BLOCKS;
    }

    if (add_blocks > K_ARRAY_SIZE(from)) {
        return MFS_EINVAL;
    }

    old_data = g.sb.data_blocks;

    /* The data section keeps a block of its own at the least. */
    if (old_data <= add_blocks + 1u) {
        return MFS_ENOSPC;
    }

    new_data = old_data - add_blocks;

    /* The blocks of the tail that an object holds have to move first. */
    for (i = new_data; i < old_data; i++) {
        if (bitmap_test(i)) {
            moves++;
        }
    }

    for (i = 0; i < moves; i++) {
        from[i] = 0;
        to[i] = MFS_NEXT_NONE;
    }

    {
        k_u32_t taken = 0;

        for (i = new_data; i < old_data; i++) {
            if (!bitmap_test(i)) {
                continue;
            }

            rc = block_alloc_below(new_data, &to[taken]);
            if (rc != MFS_OK) {
                /* What was taken goes back: the table does not grow. */
                while (taken > 0u) {
                    taken--;
                    block_release(to[taken]);
                }
                kerror("mfs: no room to grow the table of the records");
                return MFS_ENOSPC;
            }

            from[taken] = i;
            taken++;
        }
    }

    /* The map with the new blocks reaches the device before a chain does. */
    rc = bitmap_store();
    if (rc != MFS_OK) {
        return rc;
    }

    rc = dev_sync();
    if (rc != MFS_OK) {
        return rc;
    }

    /* Every chain that holds a block of the tail moves that block. */
    for (index = 1; index <= g.inode_capacity; index++) {
        struct mfs_inode inode;
        k_u32_t previous = MFS_NEXT_NONE;
        k_u32_t current;
        k_u32_t steps = 0;

        rc = inode_read(index, &inode);
        if (rc != MFS_OK) {
            return rc;
        }

        if ((inode.flags & MFS_INODE_USED) == 0u) {
            continue;
        }

        current = inode.first_block;

        while (current != MFS_NEXT_NONE && current < old_data) {
            k_u32_t next;
            k_u32_t moved;

            rc = chain_next(current, &next);
            if (rc != MFS_OK) {
                return rc;
            }

            moved = tail_new_block(from, to, moves, current);

            if (moved != MFS_NEXT_NONE) {
                k_u8_t block[MFS_BLOCK_SIZE];

                rc = dev_read(current, block);
                if (rc != MFS_OK) {
                    return rc;
                }

                rc = dev_write(moved, block);
                if (rc != MFS_OK) {
                    return rc;
                }

                if (previous == MFS_NEXT_NONE) {
                    inode.first_block = moved;
                    rc = inode_write(index, &inode);
                } else {
                    rc = chain_set_next(previous, moved);
                }

                if (rc != MFS_OK) {
                    return rc;
                }

                block_release(current);
                previous = moved;
            } else {
                previous = current;
            }

            current = next;

            if (++steps > old_data) {
                /* A loop the check has already named: the growth leaves it
                 * to the check and does not follow it for ever. */
                break;
            }
        }
    }

    /* The tail is the table now: empty records, and the map forgets it. */
    {
        k_u8_t empty[MFS_BLOCK_SIZE];

        memset(empty, 0, sizeof(empty));

        for (i = new_data; i < old_data; i++) {
            rc = dev_write(g.sb.data_start + i, empty);
            if (rc != MFS_OK) {
                return rc;
            }

            bitmap_set(i, false);
        }
    }

    g.bitmap_dirty = true;

    rc = bitmap_store();
    if (rc != MFS_OK) {
        return rc;
    }

    /* The header is the commit: until it is written the volume is its old
     * one, with a few blocks of the tail moved out of the way. */
    sb = g.sb;
    sb.data_blocks = new_data;
    sb.meta_start -= add_blocks;
    sb.meta_blocks += add_blocks;
    sb.inode_blocks += add_blocks;
    sb.inode_records = sb.inode_blocks * inode_records_per_block();
    sb.crc = mfs_priv_header_crc(&sb);

    rc = header_write(&sb);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = dev_sync();
    if (rc != MFS_OK) {
        return rc;
    }

    g.sb = sb;
    g.inode_blocks = sb.inode_blocks;
    /* The record 0 is not used, the inodes start at 1. */
    g.inode_capacity = sb.inode_records - 1u;
    /* The blocks of the chains moved: the descriptors of the open files
     * take their record again before they touch the medium. */
    g.generation++;

    kinfo("mfs: the table of the records grew to %u record(s), %u block(s) moved",
          (unsigned)g.inode_capacity, (unsigned)moves);

    return MFS_OK;
}

/* --- the directories --- */

struct dir_buffer {
    k_u8_t data[MFS_BLOCK_SIZE * 4];
    k_size_t size;
};

/*
 * The buffer of the directories. It is not on the stack: four kilobytes do
 * not fit into the stack of a task, and the filesystem is a singleton, so
 * it is not reentrant anyway.
 */
static struct dir_buffer g_dir_buffer;
static struct dir_buffer g_dir_buffer2;

/* Read the whole content of a directory into the buffer. */
static int dir_read(const struct mfs_inode *inode, struct dir_buffer *buf)
{
    k_u32_t block = inode->first_block;
    k_u32_t offset = 0;
    k_u32_t guard = 0;

    buf->size = 0;

    if ((inode->flags & MFS_INODE_DIR) == 0) {
        return MFS_ENOTDIR;
    }

    while (offset < inode->size) {
        k_size_t chunk = inode->size - offset;

        if (chunk > MFS_BLOCK_DATA) {
            chunk = MFS_BLOCK_DATA;
        }

        if (offset + chunk > sizeof(buf->data)) {
            return MFS_E2BIG;
        }

        {
            k_u8_t block_data[MFS_BLOCK_SIZE];
            int rc = dev_read(block, block_data);

            if (rc != MFS_OK) {
                return rc;
            }
            memcpy(buf->data + offset, block_data + 4, chunk);
        }

        offset += (k_u32_t)chunk;

        if (offset < inode->size) {
            /* There is more data: the chain has to go on. */
            k_u32_t next;
            int rc = chain_next(block, &next);

            if (rc != MFS_OK) {
                return rc;
            }
            if (next == MFS_NEXT_NONE) {
                return MFS_EIO;
            }
            block = next;
        }

        if (++guard > g.sb.data_blocks) {
            return MFS_EIO;
        }
    }

    buf->size = inode->size;
    return MFS_OK;
}

/* Write the whole content of a directory back. */
static int dir_write_data(const struct mfs_inode *inode, const k_u8_t *data, k_size_t size)
{
    k_u32_t block = inode->first_block;
    k_u32_t offset = 0;
    k_u32_t guard = 0;

    while (offset < size) {
        k_size_t chunk = size - offset;
        k_u8_t block_data[MFS_BLOCK_SIZE];
        int rc;

        if (chunk > MFS_BLOCK_DATA) {
            chunk = MFS_BLOCK_DATA;
        }

        memset(block_data, 0, sizeof(block_data));
        rc = dev_read(block, block_data);
        if (rc != MFS_OK) {
            return rc;
        }

        memcpy(block_data + 4, data + offset, chunk);
        rc = dev_write(block, block_data);
        if (rc != MFS_OK) {
            return rc;
        }

        offset += (k_u32_t)chunk;

        if (offset < size) {
            k_u32_t next;
            k_u32_t new_block;

            rc = chain_next(block, &next);
            if (rc != MFS_OK) {
                return rc;
            }

            if (next == MFS_NEXT_NONE) {
                k_u8_t empty[MFS_BLOCK_SIZE];

                rc = block_alloc(&new_block);
                if (rc != MFS_OK) {
                    return rc;
                }

                /*
                 * The block is new to the chain: its first four bytes are
                 * the end of it, whatever the block held before (the header
                 * of a file that was freed). A stale number there is a loop
                 * in the chain of the directory, and a loop makes every path
                 * of the volume unreadable - the bug the cleanroom found on
                 * the two hundredth file of a directory.
                 */
                memset(empty, 0, sizeof(empty));
                empty[0] = (k_u8_t)MFS_NEXT_NONE;
                empty[1] = (k_u8_t)(MFS_NEXT_NONE >> 8);
                empty[2] = (k_u8_t)(MFS_NEXT_NONE >> 16);
                empty[3] = (k_u8_t)(MFS_NEXT_NONE >> 24);

                rc = dev_write(new_block, empty);
                if (rc != MFS_OK) {
                    block_release(new_block);
                    return rc;
                }

                rc = chain_set_next(block, new_block);
                if (rc != MFS_OK) {
                    /* The block is not in any chain: the map takes it back. */
                    block_release(new_block);
                    return rc;
                }
                block = new_block;
            } else {
                block = next;
            }
        }

        if (++guard > g.sb.data_blocks) {
            return MFS_EIO;
        }
    }

    return MFS_OK;
}

/* Write the whole content of a directory from the buffer of the filesystem. */
static int dir_write(const struct mfs_inode *inode, const struct dir_buffer *buf)
{
    return dir_write_data(inode, buf->data, buf->size);
}

/* The entries of a directory. */
static int dir_entries(const struct dir_buffer *buf, struct mfs_dirent *out, k_size_t max,
                       k_size_t *count)
{
    k_size_t offset = 0;
    k_size_t found = 0;

    while (offset + sizeof(struct mfs_dirent_header) <= buf->size) {
        struct mfs_dirent entry;
        k_size_t name_len;

        memcpy(&entry.inode, buf->data + offset, sizeof(entry.inode));
        memcpy(&entry.name_len, buf->data + offset + 4, sizeof(entry.name_len));
        entry.type = buf->data[offset + 6];
        name_len = entry.name_len;

        if (name_len > MFS_NAME_MAX ||
            offset + sizeof(struct mfs_dirent_header) + name_len > buf->size) {
            return MFS_EIO;
        }

        if (found < max) {
            struct mfs_inode child;

            memcpy(entry.name, buf->data + offset + sizeof(struct mfs_dirent_header), name_len);
            entry.name[name_len] = '\0';
            entry.size = 0;
            entry.mode = 0;
            entry.owner = 0;
            entry.group = 0;

            if (inode_read(entry.inode, &child) == MFS_OK) {
                entry.size = child.size;
                entry.mode = mfs_inode_mode(&child);
                entry.owner = mfs_inode_owner(&child);
                entry.group = mfs_inode_group(&child);
            }

            out[found] = entry;
        }
        found++;

        offset += sizeof(struct mfs_dirent_header) + name_len;
    }

    if (count != NULL) {
        *count = found;
    }

    return MFS_OK;
}

/* Find an entry of a directory by its name. */
static int dir_find(const struct dir_buffer *buf, const char *name, struct mfs_dirent *out)
{
    k_size_t offset = 0;
    k_size_t name_len = strlen(name);

    while (offset + sizeof(struct mfs_dirent_header) <= buf->size) {
        struct mfs_dirent entry;
        k_size_t entry_len;

        memcpy(&entry.inode, buf->data + offset, sizeof(entry.inode));
        memcpy(&entry.name_len, buf->data + offset + 4, sizeof(entry.name_len));
        entry.type = buf->data[offset + 6];
        entry_len = sizeof(struct mfs_dirent_header) + entry.name_len;

        if (offset + entry_len > buf->size) {
            return MFS_EIO;
        }

        if (entry.name_len == name_len &&
            memcmp(buf->data + offset + sizeof(struct mfs_dirent_header), name, name_len) == 0) {
            if (out != NULL) {
                memcpy(out->name, buf->data + offset + sizeof(struct mfs_dirent_header), name_len);
                out->name[name_len] = '\0';
                out->inode = entry.inode;
                out->type = entry.type;
                out->name_len = entry.name_len;
            }
            return MFS_OK;
        }

        offset += entry_len;
    }

    return MFS_ENOENT;
}

/* Add an entry to a directory. */
static int dir_add(struct dir_buffer *buf, const struct mfs_dirent *entry)
{
    k_size_t need = sizeof(struct mfs_dirent_header) + entry->name_len;

    /*
     * The buffer of the directory is the room of the directory: a person
     * who fills it has no space left for another name, and that is what the
     * answer says. E2BIG would read as a bad argument of a call that was
     * well formed.
     */
    if (buf->size + need > sizeof(buf->data)) {
        return MFS_ENOSPC;
    }

    memcpy(buf->data + buf->size, entry, sizeof(struct mfs_dirent_header));
    memcpy(buf->data + buf->size + sizeof(struct mfs_dirent_header), entry->name, entry->name_len);
    buf->size += need;

    return MFS_OK;
}

/* Remove an entry of a directory. */
static int dir_remove(struct dir_buffer *buf, const char *name)
{
    k_size_t offset = 0;
    k_size_t name_len = strlen(name);

    while (offset + sizeof(struct mfs_dirent_header) <= buf->size) {
        struct mfs_dirent entry;
        k_size_t entry_len;

        memcpy(&entry.inode, buf->data + offset, sizeof(entry.inode));
        memcpy(&entry.name_len, buf->data + offset + 4, sizeof(entry.name_len));
        entry.type = buf->data[offset + 6];
        entry_len = sizeof(struct mfs_dirent_header) + entry.name_len;

        if (offset + entry_len > buf->size) {
            return MFS_EIO;
        }

        if (entry.name_len == name_len &&
            memcmp(buf->data + offset + sizeof(struct mfs_dirent_header), name, name_len) == 0) {
            memmove(buf->data + offset, buf->data + offset + entry_len,
                    buf->size - offset - entry_len);
            buf->size -= entry_len;
            return MFS_OK;
        }

        offset += entry_len;
    }

    return MFS_ENOENT;
}

/* --- the paths --- */

/*
 * Find the object of a path. The result is the inode of the object, the
 * entry that points to it and the number of the inode of the object itself.
 */
static int path_lookup(const char *path, struct mfs_inode *out, struct mfs_dirent *entry,
                       k_u32_t *inode_number)
{
    struct mfs_inode inode;
    struct dir_buffer *dir = &g_dir_buffer;
    struct mfs_dirent local;
    char name[MFS_NAME_MAX + 1];
    const char *cursor = path;
    k_u32_t current_inode = MFS_ROOT_INODE;
    int rc;

    if (path == NULL || path[0] != '/') {
        return MFS_EINVAL;
    }

    /* The caller does not always want the entry, the work is done anyway. */
    if (entry == NULL) {
        entry = &local;
    }

    rc = inode_read(MFS_ROOT_INODE, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    while (*cursor == '/') {
        cursor++;
    }

    if (*cursor == '\0') {
        *out = inode;
        if (inode_number != NULL) {
            *inode_number = MFS_ROOT_INODE;
        }
        if (entry != NULL) {
            memset(entry, 0, sizeof(*entry));
            entry->inode = MFS_ROOT_INODE;
            entry->type = (k_u8_t)inode.flags;
        }
        return MFS_OK;
    }

    while (*cursor != '\0') {
        k_size_t len = 0;

        while (cursor[len] != '\0' && cursor[len] != '/') {
            len++;
        }

        if (len == 0 || len > MFS_NAME_MAX) {
            return MFS_EINVAL;
        }

        memcpy(name, cursor, len);
        name[len] = '\0';

        rc = dir_read(&inode, dir);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = dir_find(dir, name, entry);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = inode_read(entry->inode, &inode);
        if (rc != MFS_OK) {
            return rc;
        }

        current_inode = entry->inode;

        cursor += len;
        while (*cursor == '/') {
            cursor++;
        }
    }

    *out = inode;
    if (inode_number != NULL) {
        *inode_number = current_inode;
    }
    return MFS_OK;
}

/* The parent of a path and the name of the object. */
static int path_parent(const char *path, char *dir, k_size_t dir_size, char *name,
                       k_size_t name_size)
{
    const char *slash;
    k_size_t len;

    if (path == NULL || path[0] != '/') {
        return MFS_EINVAL;
    }

    slash = strrchr(path, '/');
    if (slash == NULL) {
        return MFS_EINVAL;
    }

    len = (k_size_t)(slash - path);
    if (len == 0) {
        len = 1; /* the root */
    }
    if (len + 1 > dir_size) {
        return MFS_E2BIG;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';

    len = strlen(slash + 1);
    if (len == 0) {
        return MFS_EINVAL;
    }
    if (len + 1 > name_size) {
        return MFS_E2BIG;
    }
    memcpy(name, slash + 1, len + 1);

    return MFS_OK;
}

/* --- the public interface --- */

static int mfs_format_locked(const struct k_blockdev *dev, k_u32_t max_records)
{
    struct mfs_superblock sb;
    struct mfs_inode inode;
    k_u32_t total;
    k_u32_t info_blocks = 1;
    k_u32_t info_start;
    k_u32_t meta_blocks;
    k_u32_t data_blocks;
    k_u32_t bitmap_blocks;
    k_u32_t inode_blocks;
    k_u32_t records;
    k_u32_t attempt;
    k_u32_t i;
    int rc;

    if (dev == NULL || dev->ops == NULL || dev->ops->read == NULL || dev->ops->write == NULL) {
        return MFS_EINVAL;
    }

    /* The volume is written in blocks of MFS_BLOCK_SIZE, see mfs_mount(). */
    if (dev->sector_size != MFS_BLOCK_SIZE) {
        return MFS_EINVAL;
    }

    if (dev->sectors < 16) {
        return MFS_EINVAL;
    }

    if (max_records == 0) {
        max_records = 64;
    }

    total = dev->sectors;

    /*
     * The INFO section starts at the border of an erase sector of the
     * flash and covers the rest of the volume. The header of the
     * filesystem is then alone in its sector: writing the table of the
     * inodes can never erase the header together with it.
     */
    if (total < info_blocks + MFS_SECTOR_BLOCKS) {
        return MFS_ENOSPC;
    }

    info_start = total - info_blocks;
    info_start -= info_start % MFS_SECTOR_BLOCKS;
    info_blocks = total - info_start;

    /*
     * The table of the records is sized by the number of the records, not
     * by a share of the volume (ADR 0038): the records asked for and a
     * quarter of them more, 64 at the least. A table that runs full grows
     * into the data section (table_grow()).
     */
    records = max_records + max_records / MFS_RECORDS_RESERVE_DIVISOR;

    if (records - max_records < MFS_RECORDS_RESERVE) {
        records = max_records + MFS_RECORDS_RESERVE;
    }

    if (records < MFS_ROOT_INODE + 1u) {
        records = MFS_ROOT_INODE + 1u;
    }

    inode_blocks = (records * MFS_INODE_SIZE + MFS_BLOCK_SIZE - 1u) / MFS_BLOCK_SIZE;

    /*
     * The META section holds the map of the free blocks and the table, and
     * nothing else. The map depends on the size of the DATA section, which
     * depends on the size of META, so a few rounds find the layout: the
     * table is fixed and META grows by the blocks of the map only.
     */
    meta_blocks = inode_blocks + 1u;

    for (attempt = 0; attempt < 4; attempt++) {
        if (info_start <= meta_blocks) {
            return MFS_ENOSPC;
        }

        data_blocks = info_start - meta_blocks;
        bitmap_blocks = bitmap_blocks_for(data_blocks);

        /*
         * The map lives in the memory of the machine, not in the format:
         * a volume that needs more than the ceiling is refused here, with
         * the reason, before a block of it is written.
         */
        if (bitmap_blocks > MFS_BITMAP_MAX_BLOCKS) {
            kerror("mfs: a volume of %u block(s) needs %u block(s) of the map, %u is the limit",
                   (unsigned)total, (unsigned)bitmap_blocks, (unsigned)MFS_BITMAP_MAX_BLOCKS);
            return MFS_EINVAL;
        }

        if (inode_blocks + bitmap_blocks == meta_blocks) {
            break;
        }

        meta_blocks = inode_blocks + bitmap_blocks;
    }

    if (info_start <= meta_blocks) {
        return MFS_ENOSPC;
    }

    data_blocks = info_start - meta_blocks;
    bitmap_blocks = bitmap_blocks_for(data_blocks);

    if (inode_blocks + bitmap_blocks != meta_blocks) {
        return MFS_ENOSPC;
    }

    memset(&sb, 0, sizeof(sb));
    sb.magic = MFS_MAGIC;
    sb.version = MFS_VERSION;
    sb.block_size = MFS_BLOCK_SIZE;
    sb.total_blocks = total;
    sb.data_start = 0;
    sb.data_blocks = data_blocks;
    sb.meta_start = info_start - meta_blocks;
    sb.meta_blocks = meta_blocks;
    sb.info_start = info_start;
    sb.info_blocks = info_blocks;
    sb.root_inode = MFS_ROOT_INODE;
    sb.features = MFS_FEATURE_CRC | MFS_FEATURE_TIME | MFS_FEATURE_RECORD_CRC;
    /* The map sits at the top of META, the table below it. */
    sb.bitmap_blocks = bitmap_blocks;
    sb.inode_blocks = inode_blocks;
    sb.inode_size = MFS_INODE_SIZE;
    sb.inode_records = inode_blocks * (MFS_BLOCK_SIZE / MFS_INODE_SIZE);
    sb.crc = mfs_priv_header_crc(&sb);

    /* The state is needed by the helpers below. */
    bitmap_release();
    memset(&g, 0, sizeof(g));
    g.dev = dev;
    g.sb = sb;
    g.inode_size = MFS_INODE_SIZE;
    g.bitmap_blocks = bitmap_blocks;
    g.bitmap_start = sb.info_start - bitmap_blocks;
    g.inode_blocks = inode_blocks;
    g.table_end = sb.info_start - bitmap_blocks;

    rc = bitmap_alloc(bitmap_blocks);
    if (rc != MFS_OK) {
        return rc;
    }

    /* The record 0 is not used, the inodes start at 1. */
    g.inode_capacity = sb.inode_records - 1u;

    /*
     * The bitmap: a bit of zero means that the block is free. The bits of
     * the blocks that do not exist are set, so that they are never given
     * out. The map came back zeroed from the allocator.
     */
    if (data_blocks % 8u != 0u) {
        g.bitmap[bitmap_bytes() - 1] = (k_u8_t) ~((1u << (data_blocks % 8u)) - 1u);
    }

    /*
     * The table of the inodes is empty. It is written empty before the
     * header names it, so that a format that is interrupted never leaves a
     * table of stale records for a volume that is not there yet.
     */
    {
        k_u8_t empty[MFS_BLOCK_SIZE];

        memset(empty, 0, sizeof(empty));
        for (i = 0; i < inode_blocks; i++) {
            if (dev_write(sb.meta_start + i, empty) != MFS_OK) {
                return MFS_EIO;
            }
        }
    }

    /* The root directory takes the first block of the DATA section. */
    {
        k_u8_t block[MFS_BLOCK_SIZE];

        memset(block, 0, sizeof(block));
        block[0] = (k_u8_t)MFS_NEXT_NONE;
        block[1] = (k_u8_t)(MFS_NEXT_NONE >> 8);
        block[2] = (k_u8_t)(MFS_NEXT_NONE >> 16);
        block[3] = (k_u8_t)(MFS_NEXT_NONE >> 24);

        if (dev_write(0, block) != MFS_OK) {
            return MFS_EIO;
        }
    }

    bitmap_set(0, true);

    memset(&inode, 0, sizeof(inode));
    inode.flags = inode_flags(MFS_INODE_USED | MFS_INODE_DIR, MFS_MODE_DIR_DEFAULT, 0);
    inode.first_block = 0;
    inode.size = 0;
    inode.entries = 0;
    inode_stamp(&inode);

    if (inode_write(MFS_ROOT_INODE, &inode) != MFS_OK) {
        return MFS_EIO;
    }

    if (bitmap_store() != MFS_OK) {
        return MFS_EIO;
    }

    /*
     * The header goes last: a format that is interrupted half way leaves a
     * volume without a header, that is, a volume that is not there. The
     * header lives in the last block of the volume, the block the mount
     * reads.
     */
    {
        k_u8_t info_block[MFS_BLOCK_SIZE];

        memset(info_block, 0, sizeof(info_block));
        memcpy(info_block, &sb, sizeof(sb));

        if (dev_write(sb.info_start + sb.info_blocks - 1u, info_block) != MFS_OK) {
            return MFS_EIO;
        }
    }

    /*
     * The changes have to reach the device: a medium that holds its writes
     * back until it is told that the filesystem is done keeps the new
     * volume in a buffer otherwise.
     */
    return dev_sync();
}

static int mfs_mount_locked(const struct k_blockdev *dev)
{
    struct mfs_superblock sb;
    int rc;

    if (dev == NULL || dev->ops == NULL || dev->ops->read == NULL) {
        return MFS_EINVAL;
    }

    /*
     * The volume is laid out in blocks of MFS_BLOCK_SIZE and the device is
     * asked for one sector per block. A device of another sector size would
     * still "work" for a while - and then read a block at the wrong offset,
     * so the mismatch is refused here, at the door.
     */
    if (dev->sector_size != MFS_BLOCK_SIZE) {
        kerror("mfs: the device '%s' has sectors of %u bytes, %u are needed",
               dev->name != NULL ? dev->name : "?", (unsigned)dev->sector_size,
               (unsigned)MFS_BLOCK_SIZE);
        return MFS_EINVAL;
    }

    /* A mount of one volume gives up the map of the volume before it. */
    bitmap_release();
    memset(&g, 0, sizeof(g));
    g.dev = dev;

    {
        k_u8_t info_block[MFS_BLOCK_SIZE];

        rc = dev->ops->read(dev->ctx, dev->sectors - 1u, info_block, 1);
        if (rc != 0) {
            return MFS_EIO;
        }

        memcpy(&sb, info_block, sizeof(sb));
    }

    if (sb.magic != MFS_MAGIC) {
        return MFS_EINVAL;
    }

    if (sb.block_size != MFS_BLOCK_SIZE) {
        return MFS_EINVAL;
    }

    /*
     * The version says which record the table holds and where the map and
     * the table are (ADR 0013, ADR 0038). A version 1 volume has 16-byte
     * records and no time; version 2 has 32 bytes and the time; version 3
     * has 64 bytes, the time, the group and a checksum per record, and a
     * header that names the place and the count of both of them. A header
     * that claims a feature without the room, or the room without the
     * feature, describes a volume this system does not know how to read.
     */
    if (sb.version == MFS_VERSION_LEGACY) {
        if ((sb.features & MFS_FEATURE_TIME) != 0u) {
            kerror("mfs: a legacy volume claims the time it has no room for");
            return MFS_EINVAL;
        }
        g.inode_size = MFS_INODE_SIZE_LEGACY;
    } else if (sb.version == MFS_VERSION_TIME) {
        if ((sb.features & MFS_FEATURE_TIME) == 0u) {
            kerror("mfs: a volume of version %u has no time feature", (unsigned)sb.version);
            return MFS_EINVAL;
        }
        g.inode_size = MFS_INODE_SIZE_TIME;
    } else if (sb.version == MFS_VERSION) {
        if ((sb.features & (MFS_FEATURE_TIME | MFS_FEATURE_RECORD_CRC)) !=
            (MFS_FEATURE_TIME | MFS_FEATURE_RECORD_CRC)) {
            kerror("mfs: a volume of version %u has no time or no checksum of a record",
                   (unsigned)sb.version);
            return MFS_EINVAL;
        }
        if (sb.inode_size != MFS_INODE_SIZE) {
            kerror("mfs: a volume of version %u keeps records of %u bytes", (unsigned)sb.version,
                   (unsigned)sb.inode_size);
            return MFS_EINVAL;
        }
        g.inode_size = MFS_INODE_SIZE;
    } else {
        kerror("mfs: the volume is of version %u, and the system reads 1, 2 and 3",
               (unsigned)sb.version);
        return MFS_EINVAL;
    }

    if (sb.total_blocks != dev->sectors) {
        return MFS_EINVAL;
    }

    /* A header that a brownout left half written must not be trusted. */
    if ((sb.features & MFS_FEATURE_CRC) != 0u) {
        if (sb.crc != mfs_priv_header_crc(&sb)) {
            kerror("mfs: the header of the volume is damaged");
            return MFS_EINVAL;
        }
    }

    /* Every section has to have at least one block, the arithmetic below
     * works with the sizes of the sections. */
    if (sb.data_blocks == 0 || sb.meta_blocks == 0 || sb.info_blocks == 0) {
        return MFS_EINVAL;
    }

    /* The layout has to be consistent: the three sections fill the volume. */
    if (sb.data_blocks + sb.meta_blocks + sb.info_blocks != sb.total_blocks) {
        return MFS_EINVAL;
    }

    /* And the sections have to be where the design says they are. */
    if (sb.info_start + sb.info_blocks != sb.total_blocks ||
        sb.meta_start + sb.meta_blocks != sb.info_start || sb.data_start != 0) {
        return MFS_EINVAL;
    }

    /* The root of the volume is a directory, and the header says it is the
     * record 1. A volume whose header is intact but whose table of the
     * inodes is not would mount and then fail every lookup: better to say
     * that the volume is damaged and keep the data for a person. */
    if (sb.root_inode != MFS_ROOT_INODE || (sb.info_start % 4u) != 0u) {
        kerror("mfs: the header names a root that cannot exist");
        return MFS_EINVAL;
    }

    /*
     * A version 3 volume says where its map and its table are, and the two
     * have to fill META the way the format put them: the numbers of the
     * header are the numbers the mount walks, so a header that disagrees
     * with itself is refused here instead of indexing a table that is not
     * there (ADR 0038).
     */
    if (sb.version >= MFS_VERSION) {
        k_u32_t bitmap_blocks = bitmap_blocks_for(sb.data_blocks);

        if (sb.bitmap_blocks != bitmap_blocks || sb.inode_blocks == 0u ||
            sb.bitmap_blocks + sb.inode_blocks != sb.meta_blocks ||
            sb.inode_records != sb.inode_blocks * (MFS_BLOCK_SIZE / MFS_INODE_SIZE)) {
            kerror("mfs: the map and the table of the volume do not fill META");
            return MFS_EINVAL;
        }

        g.bitmap_start = sb.info_start - sb.bitmap_blocks;
        g.table_end = g.bitmap_start;
    } else {
        g.bitmap_start = sb.meta_start;
    }

    g.sb = sb;

    rc = bitmap_load();
    if (rc != MFS_OK) {
        return rc;
    }

    if (sb.version >= MFS_VERSION) {
        g.inode_blocks = sb.inode_blocks;
        g.inode_capacity = sb.inode_records - 1u;
    } else {
        /* The rest of the META section belongs to the table of the inodes. */
        if (sb.meta_blocks <= g.bitmap_blocks) {
            rc = MFS_EINVAL;
            goto fail;
        }

        g.inode_blocks = sb.meta_blocks - g.bitmap_blocks;
        /* The record 0 is not used, the inodes start at 1. */
        g.inode_capacity = (g.inode_blocks * MFS_BLOCK_SIZE) / g.inode_size - 1u;
    }

    if (g.inode_capacity < MFS_ROOT_INODE) {
        rc = MFS_EINVAL;
        goto fail;
    }

    {
        struct mfs_inode root;

        if (inode_read(MFS_ROOT_INODE, &root) != MFS_OK) {
            rc = MFS_EINVAL;
            goto fail;
        }

        if ((root.flags & (MFS_INODE_USED | MFS_INODE_DIR)) != (MFS_INODE_USED | MFS_INODE_DIR) ||
            root.first_block >= sb.data_blocks) {
            kerror("mfs: the root of the volume is not a directory (flags 0x%x)",
                   (unsigned)root.flags);
            rc = MFS_EINVAL;
            goto fail;
        }
    }

    g.mounted = true;

    return MFS_OK;

fail:
    /*
     * A volume that did not mount is not the volume that owns the map: the
     * memory goes back before the call answers.
     */
    bitmap_release();

    return rc;
}

static int mfs_sync_locked(void);

static void mfs_unmount_locked(void)
{
    /*
     * The changes that are only in the memory have to reach the flash
     * before the volume is forgotten: an unmounted volume that comes back
     * would show the state of an earlier moment. The files themselves are
     * closed by the one that opened them - the filesystem keeps no table
     * of the open files.
     */
    if (g.mounted) {
        (void)mfs_sync_locked();
    }

    bitmap_release();
    memset(&g, 0, sizeof(g));
}

static int mfs_sync_locked(void)
{
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    if (g.bitmap_dirty) {
        rc = bitmap_store();
        if (rc != MFS_OK) {
            return rc;
        }
        g.bitmap_dirty = false;
    }

    /* A device that holds its writes back learns that the work is done. */
    return dev_sync();
}

/* --- the pieces the repair pass of the check uses --- */

/*
 * A repair is a write of the filesystem like any other, so it goes through
 * the same helpers: the map of the free blocks in the memory stays the map
 * of the device, and a task that asks for a block after the repair gets a
 * block the repair freed. The check itself never calls these.
 */
void mfs_priv_bitmap_set(k_u32_t index, bool used)
{
    bitmap_set(index, used);
}

int mfs_priv_inode_write(k_u32_t index, const struct mfs_inode *inode)
{
    return inode_write(index, inode);
}

int mfs_priv_chain_set_next(k_u32_t block, k_u32_t next)
{
    return chain_set_next(block, next);
}

int mfs_priv_block_alloc(k_u32_t *block)
{
    return block_alloc(block);
}

int mfs_priv_dir_write(const struct mfs_inode *inode, const k_u8_t *data, k_size_t size)
{
    return dir_write_data(inode, data, size);
}

int mfs_priv_sync(void)
{
    return mfs_sync_locked();
}

bool mfs_is_mounted(void)
{
    return g.mounted;
}

static int mfs_get_info_locked(struct mfs_info *out)
{
    k_u32_t i;
    k_u32_t free_blocks = 0;

    if (out == NULL || !g.mounted) {
        return MFS_EINVAL;
    }

    for (i = 0; i < g.sb.data_blocks; i++) {
        if (!bitmap_test(i)) {
            free_blocks++;
        }
    }

    memset(out, 0, sizeof(*out));
    out->total_blocks = g.sb.total_blocks;
    out->data_blocks = g.sb.data_blocks;
    out->data_free = free_blocks;
    out->meta_blocks = g.sb.meta_blocks;
    out->inodes_total = g.inode_capacity;
    out->block_size = MFS_BLOCK_SIZE;

    for (i = 1; i <= g.inode_capacity; i++) {
        struct mfs_inode inode;

        /*
         * The count is of the records the table says are used, and a record
         * that does not checksum still says it is one: the count is not the
         * place to refuse the volume, and a person's df has to answer on a
         * volume that fsck will repair (ADR 0038).
         */
        if (inode_read_lenient(i, &inode) != MFS_OK) {
            break;
        }
        if (inode.flags & MFS_INODE_USED) {
            out->inodes_used++;
        }
    }

    out->inodes_free = out->inodes_total - out->inodes_used;

    return MFS_OK;
}

/* Add an object to a directory. */
static int add_to_parent(const char *dir_path, const char *name, k_u32_t inode, k_u8_t type)
{
    struct mfs_inode parent;
    struct dir_buffer *dir = &g_dir_buffer;
    struct mfs_dirent entry;
    k_u32_t dir_inode = MFS_ROOT_INODE;
    int rc;

    rc = path_lookup(dir_path, &parent, NULL, &dir_inode);
    if (rc != MFS_OK) {
        return rc;
    }

    if ((parent.flags & MFS_INODE_DIR) == 0) {
        return MFS_ENOTDIR;
    }

    rc = dir_read(&parent, dir);
    if (rc != MFS_OK) {
        return rc;
    }

    if (dir_find(dir, name, NULL) == MFS_OK) {
        return MFS_EEXIST;
    }

    memset(&entry, 0, sizeof(entry));
    entry.inode = inode;
    entry.type = type;
    entry.name_len = (k_u16_t)strlen(name);
    memcpy(entry.name, name, entry.name_len);
    entry.name[entry.name_len] = '\0';

    rc = dir_add(dir, &entry);
    if (rc != MFS_OK) {
        return rc;
    }

    parent.size = (k_u32_t)dir->size;
    parent.entries++;
    inode_stamp(&parent);
    rc = dir_write(&parent, dir);
    if (rc != MFS_OK) {
        return rc;
    }

    return inode_write(dir_inode, &parent);
}

/*
 * Whether the name is free in its parent directory, asked before anything
 * is taken for the object.
 *
 * A new directory takes a block of the data, and the map of the free blocks
 * reaches the device before the record that points at the block does. A
 * block that is taken and then given back because the name was taken after
 * all would be free in the memory and taken on the device: an object that
 * cannot be made must not take a block at all, or a power cut before the
 * next sync loses the block for good. This is how the boot of the board
 * leaked a block on every restart - it makes /log and /etc/init.d again.
 */
static int parent_name_free(const char *dir_path, const char *name)
{
    struct mfs_inode parent;
    struct dir_buffer *dir = &g_dir_buffer;
    int rc;

    rc = path_lookup(dir_path, &parent, NULL, NULL);
    if (rc != MFS_OK) {
        return rc;
    }

    if ((parent.flags & MFS_INODE_DIR) == 0) {
        return MFS_ENOTDIR;
    }

    rc = dir_read(&parent, dir);
    if (rc != MFS_OK) {
        return rc;
    }

    return (dir_find(dir, name, NULL) == MFS_OK) ? MFS_EEXIST : MFS_OK;
}

/*
 * A directory that could not be made gives its block back to the map, and
 * the map goes to the device at once: the block was taken on the device
 * before the record of the directory was written, so a release that stays
 * in the memory would be lost by a power cut.
 */
static void mkdir_give_back(k_u32_t block)
{
    block_release(block);
    (void)mfs_sync();
}

static int mfs_mkdir_locked(const char *path)
{
    char dir_path[MFS_PATH_MAX];
    char name[MFS_NAME_MAX + 1];
    struct mfs_inode inode;
    k_u32_t index;
    k_u32_t block;
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    rc = path_parent(path, dir_path, sizeof(dir_path), name, sizeof(name));
    if (rc != MFS_OK) {
        return rc;
    }

    /* The name is checked before the inode and the block are taken. */
    rc = parent_name_free(dir_path, name);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = inode_alloc(&index);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = block_alloc(&block);
    if (rc != MFS_OK) {
        return rc;
    }

    /*
     * The block is taken: the map has to reach the device before the
     * record that points at it. Otherwise a restart gives the same block
     * to somebody else, and the two objects overwrite each other.
     */
    rc = mfs_sync();
    if (rc != MFS_OK) {
        block_release(block);
        return rc;
    }

    /* The directory starts empty, the chain ends right here. */
    {
        k_u8_t empty[MFS_BLOCK_SIZE];

        memset(empty, 0, sizeof(empty));
        empty[0] = (k_u8_t)MFS_NEXT_NONE;
        empty[1] = (k_u8_t)(MFS_NEXT_NONE >> 8);
        empty[2] = (k_u8_t)(MFS_NEXT_NONE >> 16);
        empty[3] = (k_u8_t)(MFS_NEXT_NONE >> 24);

        rc = dev_write(block, empty);
        if (rc != MFS_OK) {
            mkdir_give_back(block);
            return rc;
        }
    }

    memset(&inode, 0, sizeof(inode));
    inode.flags = inode_flags(MFS_INODE_USED | MFS_INODE_DIR, MFS_MODE_DIR_DEFAULT, 0);
    inode.first_block = block;
    inode.size = 0;
    inode.entries = 0;
    inode_stamp(&inode);

    rc = inode_write(index, &inode);
    if (rc != MFS_OK) {
        mkdir_give_back(block);
        return rc;
    }

    rc = add_to_parent(dir_path, name, index, (k_u8_t)(inode.flags & MFS_INODE_TYPE_MASK));
    if (rc != MFS_OK) {
        memset(&inode, 0, sizeof(inode));
        inode_write(index, &inode);
        mkdir_give_back(block);
        return rc;
    }

    /* The new directory has to survive the restart. */
    return mfs_sync();
}

static int mfs_create_locked(const char *path)
{
    char dir_path[MFS_PATH_MAX];
    char name[MFS_NAME_MAX + 1];
    struct mfs_inode inode;
    k_u32_t index;
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    rc = path_parent(path, dir_path, sizeof(dir_path), name, sizeof(name));
    if (rc != MFS_OK) {
        return rc;
    }

    /* The name is checked before the record of the object is taken. */
    rc = parent_name_free(dir_path, name);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = inode_alloc(&index);
    if (rc != MFS_OK) {
        return rc;
    }

    memset(&inode, 0, sizeof(inode));
    inode.flags = inode_flags(MFS_INODE_USED, MFS_MODE_DEFAULT, 0);
    inode.first_block = MFS_NEXT_NONE;
    inode.size = 0;
    inode.entries = 0;
    inode_stamp(&inode);

    rc = inode_write(index, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = add_to_parent(dir_path, name, index, (k_u8_t)(inode.flags & MFS_INODE_TYPE_MASK));
    if (rc != MFS_OK) {
        memset(&inode, 0, sizeof(inode));
        inode_write(index, &inode);
        return rc;
    }

    /* The new object has to survive the restart. */
    return mfs_sync();
}

static int mfs_open_locked(const char *path, int flags, struct mfs_file *out)
{
    struct mfs_inode inode;
    struct mfs_dirent entry;
    int rc;

    if (!g.mounted || out == NULL) {
        return MFS_EINVAL;
    }

    rc = path_lookup(path, &inode, &entry, NULL);
    if (rc == MFS_ENOENT && (flags & MFS_O_CREAT) != 0) {
        rc = mfs_create(path);
        if (rc != MFS_OK) {
            return rc;
        }
        rc = path_lookup(path, &inode, &entry, NULL);
    }

    if (rc != MFS_OK) {
        return rc;
    }

    if ((inode.flags & MFS_INODE_DIR) != 0) {
        /*
         * A directory is opened only as a directory, only to be read and
         * only to have its entries walked: a descriptor that would write to
         * it or cut it has no meaning here. A plain open of a directory is
         * still EISDIR, the answer it has always been.
         */
        if ((flags & MFS_O_DIRECTORY) == 0) {
            return MFS_EISDIR;
        }

        if ((flags & 0x3) != MFS_O_RDONLY ||
            (flags & (MFS_O_CREAT | MFS_O_TRUNC | MFS_O_APPEND)) != 0) {
            return MFS_EINVAL;
        }

        memset(out, 0, sizeof(*out));
        file_cursor_forget(out);
        out->inode = entry.inode;
        out->first_block = inode.first_block;
        out->size = inode.size;
        out->offset = 0;
        out->flags = flags;
        out->generation = g.generation;

        return MFS_OK;
    }

    if ((flags & MFS_O_DIRECTORY) != 0) {
        return MFS_ENOTDIR;
    }

    if ((flags & MFS_O_TRUNC) != 0 && (flags & (MFS_O_WRONLY | MFS_O_RDWR)) != 0) {
        /*
         * The content of the file is dropped: the blocks of the chain go
         * back to the map, otherwise they are lost forever.
         */
        rc = file_release_chain(inode.first_block);
        if (rc != MFS_OK) {
            return rc;
        }

        inode.size = 0;
        inode.first_block = MFS_NEXT_NONE;
        inode_stamp(&inode);

        rc = inode_write(entry.inode, &inode);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = mfs_sync();
        if (rc != MFS_OK) {
            return rc;
        }
    }

    memset(out, 0, sizeof(*out));
    file_cursor_forget(out);
    out->inode = entry.inode;
    out->first_block = inode.first_block;
    out->size = inode.size;
    /*
     * A file that was opened with "append" starts at its end: everything
     * written to it is added to what is already there.
     */
    out->offset = ((flags & MFS_O_APPEND) != 0) ? inode.size : 0;
    out->flags = flags;

    return MFS_OK;
}

static int mfs_read_locked(struct mfs_file *file, void *buf, k_size_t size)
{
    k_u8_t *out = (k_u8_t *)buf;
    k_size_t done = 0;
    int rc;

    if (file == NULL || buf == NULL) {
        return MFS_EINVAL;
    }

    rc = file_refresh(file);
    if (rc != MFS_OK) {
        return rc;
    }

    /*
     * The descriptor says what the file was opened for: a file that was
     * opened to be written to cannot be read from.
     */
    if ((file->flags & 0x3) == MFS_O_WRONLY) {
        return MFS_EINVAL;
    }

    if (file->offset >= file->size) {
        return 0;
    }

    if (file->offset + size > file->size) {
        size = file->size - file->offset;
    }

    while (done < size) {
        k_u32_t block;
        k_u32_t in_block;
        k_size_t chunk;

        rc = file_block_at(file, file->offset, &block, &in_block);
        if (rc != MFS_OK) {
            return rc;
        }

        chunk = MFS_BLOCK_DATA - in_block;
        if (chunk > size - done) {
            chunk = size - done;
        }

        {
            k_u8_t block_data[MFS_BLOCK_SIZE];
            k_u32_t next;

            rc = dev_read(block, block_data);
            if (rc != MFS_OK) {
                return rc;
            }
            memcpy(out + done, block_data + 4 + in_block, chunk);

            /*
             * The header of the block names the next one, so the block that
             * was just read is the step of the chain the next chunk needs:
             * without it `file_block_at` asks `chain_next` for a block it has
             * already read and a read of n blocks costs two reads per block.
             * The cursor of the descriptor is moved to that block, and the
             * walk of the call resumes from there.
             */
            memcpy(&next, block_data, sizeof(next));
            if (done + chunk < size) {
                if (next == MFS_NEXT_NONE) {
                    return MFS_EIO;
                }
                file->chain_block = next;
                file->chain_index = (file->offset + (k_u32_t)chunk) / MFS_BLOCK_DATA;
                file->chain_valid = true;
            }
        }

        done += chunk;
        file->offset += (k_u32_t)chunk;
    }

    return (int)done;
}

/* Append a block to the end of the chain of a file. */
static int file_grow(struct mfs_file *file, k_u32_t *new_block)
{
    k_u32_t last;
    k_u32_t block;
    k_u8_t empty[MFS_BLOCK_SIZE];
    int rc;

    rc = block_alloc(&block);
    if (rc != MFS_OK) {
        return rc;
    }

    /* The new block starts empty and ends the chain. */
    memset(empty, 0, sizeof(empty));
    empty[0] = (k_u8_t)MFS_NEXT_NONE;
    empty[1] = (k_u8_t)(MFS_NEXT_NONE >> 8);
    empty[2] = (k_u8_t)(MFS_NEXT_NONE >> 16);
    empty[3] = (k_u8_t)(MFS_NEXT_NONE >> 24);

    rc = dev_write(block, empty);
    if (rc != MFS_OK) {
        block_release(block);
        return rc;
    }

    if (file->first_block == MFS_NEXT_NONE) {
        /* The first block of the file: the chain has a head that the
         * cursor of an earlier chain does not name. */
        file->first_block = block;
        file_cursor_forget(file);
        file->chain_block = block;
        file->chain_index = 0;
        file->chain_valid = true;
        file->grow_block = block;
        file->grow_index = 0;
        file->grow_valid = true;
        *new_block = block;
        return MFS_OK;
    }

    /*
     * The block that ends the chain. The append cursor names it when the
     * last growth left it there, and the count of the blocks says which
     * index that is; otherwise the chain is walked once and the rest of
     * the growth of this call goes from the new block.
     *
     * The position cursor of the descriptor is not the one asked: a write
     * grows the chain to the end of the call before it writes the first
     * byte, so the position is *behind* the tail, and a growth that moved
     * it made every call walk the chain from the head - the write of a
     * megabyte cost the square of the blocks of the file.
     */
    if (file->grow_valid && file->grow_index + 1u == file->chain_blocks) {
        last = file->grow_block;
    } else {
        rc = chain_last(file->first_block, &last);
        if (rc != MFS_OK) {
            block_release(block);
            return rc;
        }
    }

    rc = chain_set_next(last, block);
    if (rc != MFS_OK) {
        block_release(block);
        return rc;
    }

    /* The new block ends the chain: the append cursor follows it. */
    file->grow_block = block;
    file->grow_index = file->chain_blocks;
    file->grow_valid = true;

    *new_block = block;
    return MFS_OK;
}

static int mfs_write_locked(struct mfs_file *file, const void *buf, k_size_t size)
{
    const k_u8_t *in = (const k_u8_t *)buf;
    k_size_t done = 0;
    k_u32_t old_blocks = 0;
    int rc;

    if (file == NULL || buf == NULL) {
        return MFS_EINVAL;
    }

    rc = file_refresh(file);
    if (rc != MFS_OK) {
        return rc;
    }

    if ((file->flags & (MFS_O_WRONLY | MFS_O_RDWR)) == 0) {
        return MFS_EINVAL;
    }

    /*
     * A file that was opened with "append" is written to its end every
     * time: the position of the descriptor does not decide where the data
     * goes.
     */
    if ((file->flags & MFS_O_APPEND) != 0) {
        file->offset = file->size;
    }

    /* The file has to have enough blocks for the whole write. */
    {
        k_u32_t needed = (file->offset + size + MFS_BLOCK_DATA - 1u) / MFS_BLOCK_DATA;

        /*
         * The number of the blocks of the chain is counted once for the
         * descriptor, not once per call: a program that writes a file a
         * kilobyte at a time would otherwise walk the whole chain before
         * every call, which is the write side of the cost the read cursor
         * above removed.
         */
        if (!file->chain_blocks_valid) {
            k_u32_t have = 0;

            if (file->first_block != MFS_NEXT_NONE) {
                rc = chain_count(file->first_block, &have);
                if (rc != MFS_OK) {
                    return rc;
                }
            }

            file->chain_blocks = have;
            file->chain_blocks_valid = true;
        }

        old_blocks = file->chain_blocks;

        while (file->chain_blocks < needed) {
            k_u32_t added;

            rc = file_grow(file, &added);
            if (rc != MFS_OK) {
                /*
                 * The blocks that were already added belong to the file
                 * now: the close has to know about them, or they are lost.
                 */
                file->dirty = true;
                return rc;
            }
            file->chain_blocks++;
        }

        /* The blocks of the file have been taken, the map has to know it. */
        file->dirty = true;
    }

    while (done < size) {
        k_u32_t block = MFS_NEXT_NONE;
        k_u32_t in_block = 0;
        k_u32_t index;
        k_size_t chunk;
        k_u8_t block_data[MFS_BLOCK_SIZE];

        rc = file_block_at(file, file->offset, &block, &in_block);
        if (rc != MFS_OK) {
            return rc;
        }

        chunk = MFS_BLOCK_DATA - in_block;
        if (chunk > size - done) {
            chunk = size - done;
        }

        /*
         * A block that was just allocated has no old content, but the
         * pointer to the next block of the chain has to stay: it is the
         * first four bytes of the block.
         */
        index = file->offset / MFS_BLOCK_DATA;
        rc = dev_read(block, block_data);
        if (rc != MFS_OK) {
            return rc;
        }

        if (index >= old_blocks) {
            memset(block_data + 4, 0, sizeof(block_data) - 4);
        }

        memcpy(block_data + 4 + in_block, in + done, chunk);
        rc = dev_write(block, block_data);
        if (rc != MFS_OK) {
            return rc;
        }

        done += chunk;
        file->offset += (k_u32_t)chunk;

        if (file->offset > file->size) {
            file->size = file->offset;
        }
    }

    file->dirty = true;
    return (int)done;
}

static int mfs_close_locked(struct mfs_file *file)
{
    struct mfs_inode inode;
    int rc;

    if (file == NULL) {
        return MFS_EINVAL;
    }

    rc = file_refresh(file);
    if (rc != MFS_OK) {
        return rc;
    }

    if (file->dirty) {
        rc = inode_read(file->inode, &inode);
        if (rc != MFS_OK) {
            return rc;
        }
        inode.size = file->size;
        inode.first_block = file->first_block;
        inode_stamp(&inode);
        rc = inode_write(file->inode, &inode);
        if (rc != MFS_OK) {
            return rc;
        }
        file->dirty = false;
    }

    /* The blocks of the file have been taken, the map has to know it. */
    return mfs_sync();
}

/*
 * Cut the chain of an open file after the block that holds `new_size` and
 * give the tail back. The bytes past the new size in the last block are
 * zeroed, because a later grow must read zeros there, exactly as a file of
 * POSIX does.
 */
static int truncate_shrink(struct mfs_file *file, k_u32_t new_size, k_u32_t needed)
{
    k_u32_t last;
    k_u32_t tail = MFS_NEXT_NONE;
    k_u32_t in_block;
    int rc;

    if (needed == 0) {
        rc = file_release_chain(file->first_block);
        if (rc == MFS_OK) {
            file->first_block = MFS_NEXT_NONE;
            file_cursor_forget(file);
        }

        return rc;
    }

    /* The last block that stays, found through the cursor of the file. */
    rc = file_block_at(file, (needed - 1u) * MFS_BLOCK_DATA, &last, &in_block);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = chain_next(last, &tail);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = chain_set_next(last, MFS_NEXT_NONE);
    if (rc != MFS_OK) {
        return rc;
    }

    if (tail != MFS_NEXT_NONE) {
        rc = file_release_chain(tail);
        if (rc != MFS_OK) {
            return rc;
        }
    }

    in_block = new_size % MFS_BLOCK_DATA;
    if (in_block != 0) {
        k_u8_t block_data[MFS_BLOCK_SIZE];

        rc = dev_read(last, block_data);
        if (rc != MFS_OK) {
            return rc;
        }

        memset(block_data + 4 + in_block, 0, MFS_BLOCK_DATA - in_block);

        rc = dev_write(last, block_data);
        if (rc != MFS_OK) {
            return rc;
        }
    }

    if (file->chain_valid && file->chain_index >= needed) {
        file_cursor_forget(file);
    }

    return MFS_OK;
}

/*
 * Take the blocks a larger file needs and zero the bytes between the old
 * size and the new one. A new block is written empty by file_grow, so only
 * the tail of the block that held the old size needs the zero; the rest of
 * the growth reads zeros already.
 */
static int truncate_grow(struct mfs_file *file, k_u32_t new_size, k_u32_t needed)
{
    k_u32_t old_size = file->size;
    k_u32_t old_needed = (old_size + MFS_BLOCK_DATA - 1u) / MFS_BLOCK_DATA;
    int rc;

    if (!file->chain_blocks_valid) {
        k_u32_t have = 0;

        if (file->first_block != MFS_NEXT_NONE) {
            rc = chain_count(file->first_block, &have);
            if (rc != MFS_OK) {
                return rc;
            }
        }

        file->chain_blocks = have;
        file->chain_blocks_valid = true;
    }

    while (file->chain_blocks < needed) {
        k_u32_t added;

        rc = file_grow(file, &added);
        if (rc != MFS_OK) {
            /* The file is what it was: the blocks that were already taken
             * go back, or a refusal would leave a chain longer than the
             * size the close is going to write. */
            (void)truncate_shrink(file, old_size, old_needed);
            file->chain_blocks = old_needed;
            file->chain_blocks_valid = true;
            return rc;
        }

        file->chain_blocks++;
    }

    if (old_size % MFS_BLOCK_DATA != 0) {
        k_u32_t block;
        k_u32_t in_block;
        k_size_t gap = K_MIN((k_size_t)(MFS_BLOCK_DATA - old_size % MFS_BLOCK_DATA),
                             (k_size_t)(new_size - old_size));

        rc = file_block_at(file, old_size, &block, &in_block);
        if (rc != MFS_OK) {
            return rc;
        }

        {
            k_u8_t block_data[MFS_BLOCK_SIZE];

            rc = dev_read(block, block_data);
            if (rc != MFS_OK) {
                return rc;
            }

            memset(block_data + 4 + in_block, 0, gap);

            rc = dev_write(block, block_data);
            if (rc != MFS_OK) {
                return rc;
            }
        }
    }

    return MFS_OK;
}

static int mfs_truncate_locked(struct mfs_file *file, k_u64_t size)
{
    struct mfs_inode inode;
    k_u32_t new_size;
    k_u32_t needed;
    int rc;

    if (file == NULL || (file->flags & (MFS_O_WRONLY | MFS_O_RDWR)) == 0) {
        return MFS_EINVAL;
    }

    rc = file_refresh(file);
    if (rc != MFS_OK) {
        return rc;
    }

    /* The format keeps the size of a file in 32 bits, so a wider number is
     * refused before a byte is touched rather than cut to fit. */
    if (size > 0xFFFFFFFFu) {
        return MFS_E2BIG;
    }

    new_size = (k_u32_t)size;
    if (new_size == file->size) {
        return MFS_OK;
    }

    needed = (new_size + MFS_BLOCK_DATA - 1u) / MFS_BLOCK_DATA;

    if (new_size < file->size) {
        rc = truncate_shrink(file, new_size, needed);
    } else {
        rc = truncate_grow(file, new_size, needed);
    }

    if (rc != MFS_OK) {
        return rc;
    }

    rc = inode_read(file->inode, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    inode.size = new_size;
    inode.first_block = file->first_block;
    inode_stamp(&inode);

    rc = inode_write(file->inode, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    file->size = new_size;
    file->dirty = false;
    file->chain_blocks = needed;
    file->chain_blocks_valid = true;

    return mfs_sync();
}

/*
 * The removal of one name. `directory` says which of the two calls of POSIX
 * the name is: unlink of a directory is MFS_EISDIR, rmdir of a file is
 * MFS_ENOTDIR, and only rmdir looks inside for the entries that would make
 * the directory non-empty. The two calls share the rest of the work, because
 * the order of the writes is the same and a second copy of it would be a
 * second place for the order to drift.
 */
static int mfs_remove_locked(const char *path, bool directory)
{
    char dir_path[MFS_PATH_MAX];
    char name[MFS_NAME_MAX + 1];
    struct mfs_inode parent;
    struct mfs_inode inode;
    struct mfs_inode free_inode;
    struct dir_buffer *dir = &g_dir_buffer;
    struct mfs_dirent entry;
    k_u32_t parent_inode = MFS_ROOT_INODE;
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    rc = path_parent(path, dir_path, sizeof(dir_path), name, sizeof(name));
    if (rc != MFS_OK) {
        return rc;
    }

    rc = path_lookup(dir_path, &parent, NULL, &parent_inode);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = dir_read(&parent, dir);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = dir_find(dir, name, &entry);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = inode_read(entry.inode, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    if ((inode.flags & MFS_INODE_DIR) != 0) {
        struct dir_buffer *content = &g_dir_buffer2;

        if (!directory) {
            return MFS_EISDIR;
        }

        rc = dir_read(&inode, content);
        if (rc != MFS_OK) {
            return rc;
        }
        if (content->size > 0) {
            return MFS_ENOTEMPTY;
        }
    } else if (directory) {
        return MFS_ENOTDIR;
    }

    /* The record of the object is not needed any more. */
    memset(&free_inode, 0, sizeof(free_inode));

    /*
     * The entry goes away first: a directory that no longer points at the
     * object can be survived, the opposite cannot - the blocks would be
     * given to somebody else while the entry is still there.
     */
    rc = dir_remove(dir, name);
    if (rc != MFS_OK) {
        return rc;
    }

    parent.size = (k_u32_t)dir->size;
    if (parent.entries > 0) {
        parent.entries--;
    }
    inode_stamp(&parent);

    rc = dir_write(&parent, dir);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = inode_write(parent_inode, &parent);
    if (rc != MFS_OK) {
        return rc;
    }

    /* The blocks of the object are free again. */
    rc = file_release_chain(inode.first_block);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = inode_write(entry.inode, &free_inode);
    if (rc != MFS_OK) {
        return rc;
    }

    return mfs_sync();
}

/*
 * A change of the mode or of the owner is a change of the record of the
 * object: the new flags are written into the table and the volume is
 * synced, so that the numbers survive the restart like the size of a
 * file does.
 */
/*
 * The same object under another name (ADR 0024 §5). A name that is already
 * there and names a file is replaced, the way rename of POSIX does it: the
 * installer writes the new bytes beside the old program and makes them
 * visible with one call. A name of a directory is not replaced (MFS_EISDIR),
 * because the tree below it would be lost.
 *
 * A file may move to another directory of the volume; a directory stays in
 * its parent, because its own `..` and the tree below it would have to move
 * with it and this format keeps no parent cluster to rewrite.
 *
 * The new entry is written before the old one is taken away: the worst a
 * power cut between the two can do is a second name of one object, which the
 * check finds, and never an object that no name reaches.
 */
static int mfs_rename_locked(const char *from, const char *to)
{
    char from_dir_path[MFS_PATH_MAX];
    char from_name[MFS_NAME_MAX + 1];
    char to_dir_path[MFS_PATH_MAX];
    char to_name[MFS_NAME_MAX + 1];
    struct mfs_inode from_parent;
    struct mfs_inode to_parent;
    struct mfs_inode inode;
    struct mfs_inode replaced;
    struct dir_buffer *dir = &g_dir_buffer;
    struct dir_buffer *other = &g_dir_buffer2;
    struct mfs_dirent entry;
    k_u32_t from_parent_inode = MFS_ROOT_INODE;
    k_u32_t to_parent_inode = MFS_ROOT_INODE;
    bool same_dir;
    bool replacing = false;
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    if (strcmp(from, to) == 0) {
        return MFS_OK;
    }

    rc = path_parent(from, from_dir_path, sizeof(from_dir_path), from_name, sizeof(from_name));
    if (rc != MFS_OK) {
        return rc;
    }

    rc = path_parent(to, to_dir_path, sizeof(to_dir_path), to_name, sizeof(to_name));
    if (rc != MFS_OK) {
        return rc;
    }

    rc = path_lookup(from, &inode, &entry, NULL);
    if (rc != MFS_OK) {
        return rc;
    }

    /* The caller may replace a file and may not replace a directory: the
     * name of a directory that somebody still has open would take the tree
     * below it with it, and a name that is a file is what an install
     * replaces. */
    if (path_lookup(to, &replaced, NULL, NULL) == MFS_OK) {
        if ((replaced.flags & MFS_INODE_DIR) != 0) {
            return MFS_EISDIR;
        }

        replacing = true;
    }

    same_dir = strcmp(from_dir_path, to_dir_path) == 0;

    if (!same_dir && (inode.flags & MFS_INODE_DIR) != 0) {
        return MFS_ENOTSUP;
    }

    rc = path_lookup(from_dir_path, &from_parent, NULL, &from_parent_inode);
    if (rc != MFS_OK) {
        return rc;
    }

    if (same_dir) {
        rc = dir_read(&from_parent, dir);
        if (rc != MFS_OK) {
            return rc;
        }

        if (replacing) {
            rc = dir_remove(dir, to_name);
            if (rc != MFS_OK) {
                return rc;
            }
        }

        rc = dir_remove(dir, from_name);
        if (rc != MFS_OK) {
            return rc;
        }

        entry.name_len = (k_u16_t)strlen(to_name);
        memcpy(entry.name, to_name, entry.name_len);
        entry.name[entry.name_len] = '\0';

        rc = dir_add(dir, &entry);
        if (rc != MFS_OK) {
            return rc;
        }

        from_parent.size = (k_u32_t)dir->size;
        inode_stamp(&from_parent);

        rc = dir_write(&from_parent, dir);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = inode_write(from_parent_inode, &from_parent);
        if (rc != MFS_OK) {
            return rc;
        }
    } else {
        rc = path_lookup(to_dir_path, &to_parent, NULL, &to_parent_inode);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = dir_read(&to_parent, other);
        if (rc != MFS_OK) {
            return rc;
        }

        if (replacing) {
            rc = dir_remove(other, to_name);
            if (rc != MFS_OK) {
                return rc;
            }

            to_parent.entries--;
        }

        entry.name_len = (k_u16_t)strlen(to_name);
        memcpy(entry.name, to_name, entry.name_len);
        entry.name[entry.name_len] = '\0';

        rc = dir_add(other, &entry);
        if (rc != MFS_OK) {
            return rc;
        }

        to_parent.size = (k_u32_t)other->size;
        to_parent.entries++;
        inode_stamp(&to_parent);

        rc = dir_write(&to_parent, other);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = inode_write(to_parent_inode, &to_parent);
        if (rc != MFS_OK) {
            return rc;
        }

        /* The name is in its new directory: the old one may forget it. */
        rc = dir_read(&from_parent, dir);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = dir_remove(dir, from_name);
        if (rc != MFS_OK) {
            return rc;
        }

        from_parent.size = (k_u32_t)dir->size;
        if (from_parent.entries > 0) {
            from_parent.entries--;
        }
        inode_stamp(&from_parent);

        rc = dir_write(&from_parent, dir);
        if (rc != MFS_OK) {
            return rc;
        }

        rc = inode_write(from_parent_inode, &from_parent);
        if (rc != MFS_OK) {
            return rc;
        }
    }

    /* The record of the object did not change its content, only its name:
     * ctime moves and mtime stays. */
    inode_stamp_record(&inode);

    rc = inode_write(entry.inode, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    return mfs_sync();
}

static int mfs_set_attr_locked(const char *path, k_u32_t mask, k_u32_t value)
{
    struct mfs_inode inode;
    k_u32_t index = 0;
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    rc = path_lookup(path, &inode, NULL, &index);
    if (rc != MFS_OK) {
        return rc;
    }

    inode.flags = (inode.flags & ~mask) | (value & mask);
    /* The content did not change; the record did, so ctime moves. */
    inode_stamp_record(&inode);

    rc = inode_write(index, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    return mfs_sync();
}

static int mfs_chmod_locked(const char *path, k_u32_t mode)
{
    /*
     * The mode has to fit into its nine bits: a number with more would
     * spill into the owner and the record would say something else than
     * the caller asked for.
     *
     * A zero is refused as well: a volume of 0.3.x keeps its mode in the
     * same bits, where a zero means "the record predates the modes", and
     * mfs_inode_mode() reads it as the mode of the design. A "chmod 000"
     * that was written would come back as 0644, so the call says what it
     * cannot do instead of storing a number it does not keep.
     */
    if ((mode & ~MFS_MODE_MASK) != 0u || mode == 0u) {
        return MFS_EINVAL;
    }

    return mfs_set_attr_locked(path, MFS_MODE_MASK << MFS_MODE_SHIFT, mode << MFS_MODE_SHIFT);
}

static int mfs_chown_locked(const char *path, k_u32_t owner)
{
    /* The same for the fifteen bits of a uid. */
    if ((owner & ~MFS_OWNER_MASK) != 0u) {
        return MFS_EINVAL;
    }

    return mfs_set_attr_locked(path, MFS_OWNER_MASK << MFS_OWNER_SHIFT, owner << MFS_OWNER_SHIFT);
}

/*
 * The group of an object is a word of its own and not a bit of the flags,
 * so it is set on the record itself: only a version 3 volume keeps one, and
 * a volume of version 1 or 2 answers that the number would not come back
 * (ADR 0038).
 */
static int mfs_chgrp_locked(const char *path, k_u32_t group)
{
    struct mfs_inode inode;
    k_u32_t index = 0;
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    if (g.sb.version < MFS_VERSION) {
        return MFS_ENOTSUP;
    }

    rc = path_lookup(path, &inode, NULL, &index);
    if (rc != MFS_OK) {
        return rc;
    }

    inode.group = group;
    inode_stamp_record(&inode);

    rc = inode_write(index, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    return mfs_sync();
}

static int mfs_stat_locked(const char *path, struct mfs_inode *out, struct mfs_dirent *dirent)
{
    struct mfs_inode inode;
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    rc = path_lookup(path, &inode, dirent, NULL);
    if (rc != MFS_OK) {
        return rc;
    }

    /* The entry describes the same object as the record does. */
    if (dirent != NULL) {
        dirent->size = inode.size;
        dirent->mode = mfs_inode_mode(&inode);
        dirent->owner = mfs_inode_owner(&inode);
        dirent->group = mfs_inode_group(&inode);
    }

    if (out != NULL) {
        *out = inode;
    }

    return MFS_OK;
}

static int mfs_list_locked(const char *path, struct mfs_dirent *out, k_size_t max)
{
    struct mfs_inode inode;
    struct dir_buffer *dir = &g_dir_buffer;
    k_size_t count = 0;
    int rc;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    rc = path_lookup(path, &inode, NULL, NULL);
    if (rc != MFS_OK) {
        return rc;
    }

    if ((inode.flags & MFS_INODE_DIR) == 0) {
        return MFS_ENOTDIR;
    }

    rc = dir_read(&inode, dir);
    if (rc != MFS_OK) {
        return rc;
    }

    return dir_entries(dir, out, max, &count);
}

/*
 * The next entries of an open directory. The directory is read whole into
 * the buffer of the filesystem and the position of the descriptor is a byte
 * offset into its serialized entries: a call continues where the previous
 * one stopped, and a directory that grew or shrank between the calls is
 * seen as it is at the moment of the call, not as it was at the open.
 */
static int mfs_readdir_locked(struct mfs_file *file, struct mfs_dirent *out, k_size_t max,
                              k_size_t *count)
{
    struct mfs_inode inode;
    struct dir_buffer *dir = &g_dir_buffer;
    k_size_t offset;
    k_size_t found = 0;
    int rc;

    if (file == NULL || out == NULL || count == NULL) {
        return MFS_EINVAL;
    }

    *count = 0;

    if (!g.mounted) {
        return MFS_EINVAL;
    }

    rc = inode_read(file->inode, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    if ((inode.flags & MFS_INODE_DIR) == 0) {
        return MFS_ENOTDIR;
    }

    rc = dir_read(&inode, dir);
    if (rc != MFS_OK) {
        return rc;
    }

    offset = file->offset;
    while (found < max && offset + sizeof(struct mfs_dirent_header) <= dir->size) {
        struct mfs_dirent entry;
        struct mfs_inode child;
        k_size_t name_len;

        memset(&entry, 0, sizeof(entry));
        memcpy(&entry.inode, dir->data + offset, sizeof(entry.inode));
        memcpy(&entry.name_len, dir->data + offset + 4, sizeof(entry.name_len));
        entry.type = dir->data[offset + 6];
        name_len = entry.name_len;

        if (name_len > MFS_NAME_MAX ||
            offset + sizeof(struct mfs_dirent_header) + name_len > dir->size) {
            return MFS_EIO;
        }

        memcpy(entry.name, dir->data + offset + sizeof(struct mfs_dirent_header), name_len);
        entry.name[name_len] = '\0';

        /*
         * The entry of a directory does not repeat the numbers of its
         * object; a listing reports them, so the record of the child is
         * read here. A broken pointer leaves the zeros of "unknown".
         */
        if (inode_read(entry.inode, &child) == MFS_OK) {
            entry.size = child.size;
            entry.mode = mfs_inode_mode(&child);
            entry.owner = mfs_inode_owner(&child);
            entry.group = mfs_inode_group(&child);
        }

        out[found++] = entry;
        offset += sizeof(struct mfs_dirent_header) + name_len;
    }

    file->offset = (k_u32_t)offset;
    *count = found;

    return MFS_OK;
}

/*
 * The public functions of the filesystem hold the lock of the volume
 * for the whole of their work: the volume is one piece of state and
 * two tasks that touch it together corrupt it.
 */
int mfs_format(const struct k_blockdev *dev, k_u32_t max_records)
{
    int rc;

    mfs_lock();
    rc = mfs_format_locked(dev, max_records);

    /*
     * A format is not a mount: the map it built to lay the volume out is
     * given back here, so that a volume leaves the allocator as it found
     * it, whatever the layout ended with. A mount allocates its own.
     */
    bitmap_release();
    mfs_unlock();

    return rc;
}

int mfs_mount(const struct k_blockdev *dev)
{
    int rc;

    mfs_lock();
    rc = mfs_mount_locked(dev);
    mfs_unlock();

    return rc;
}

void mfs_unmount(void)
{
    mfs_lock();
    mfs_unmount_locked();
    mfs_unlock();
}

int mfs_sync(void)
{
    int rc;

    mfs_lock();
    rc = mfs_sync_locked();
    mfs_unlock();

    return rc;
}

int mfs_get_info(struct mfs_info *out)
{
    int rc;

    mfs_lock();
    rc = mfs_get_info_locked(out);
    mfs_unlock();

    return rc;
}

/*
 * The view of the mounted volume that the check and the repair walk. The
 * caller holds the lock, so the pointers are the live state and nothing
 * changes under the walk.
 */
static void checker_view(struct mfs_private *view)
{
    memset(view, 0, sizeof(*view));
    view->sb = &g.sb;
    view->bitmap = g.bitmap;
    view->dir_scratch = g_dir_buffer.data;
    view->dir_scratch_size = sizeof(g_dir_buffer.data);
    view->bitmap_blocks = g.bitmap_blocks;
    view->inode_blocks = g.inode_blocks;
    view->inode_capacity = g.inode_capacity;
    view->inode_size = g.inode_size;
    view->device_blocks = g.dev->sectors;
    view->table_end = g.table_end;
}

int mfs_check(struct mfs_check_report *report)
{
    struct mfs_private view;
    int rc;

    if (report == NULL) {
        return MFS_EINVAL;
    }

    mfs_lock();

    if (!g.mounted) {
        mfs_unlock();
        return MFS_EINVAL;
    }

    memset(report, 0, sizeof(*report));
    report->data_blocks = g.sb.data_blocks;

    /*
     * The checker gets the state of the volume, not a copy of it: the lock
     * is held for the whole walk, so nothing changes under it, and the
     * buffer of the directories is free for its use.
     */
    checker_view(&view);

    rc = mfs_check_locked(&view, report);

    mfs_unlock();

    return rc;
}

int mfs_repair(struct mfs_check_report *report)
{
    struct mfs_private view;
    struct mfs_check_report pass;
    struct mfs_check_report after;
    int rc;

    if (report == NULL) {
        return MFS_EINVAL;
    }

    mfs_lock();

    if (!g.mounted) {
        mfs_unlock();
        return MFS_EINVAL;
    }

    memset(report, 0, sizeof(*report));
    report->data_blocks = g.sb.data_blocks;

    checker_view(&view);

    /*
     * The pass writes with the state of the mount, so it runs before the
     * report is made: the counters below are the volume as the pass left
     * it, which is what a person wants to read after a repair.
     */
    memset(&pass, 0, sizeof(pass));
    pass.data_blocks = g.sb.data_blocks;
    rc = mfs_repair_locked(&view, &pass);
    if (rc != MFS_OK) {
        mfs_unlock();
        return rc;
    }

    /*
     * The check runs once more, with a map of its own, so the answer is
     * about the medium after the repair and not about the plan of the pass.
     */
    memset(&after, 0, sizeof(after));
    after.data_blocks = g.sb.data_blocks;
    rc = mfs_check_locked(&view, &after);
    if (rc != MFS_OK) {
        mfs_unlock();
        return rc;
    }

    *report = after;
    report->fixed = pass.fixed;
    report->unfixed = after.errors;

    /*
     * A pass that could not do its work - a medium that refused a write, a
     * table too big to map - says so even when the walk of the volume finds
     * nothing to name: the volume is not proven whole, and a person has to
     * know why.
     */
    if (after.errors == 0 && pass.errors != 0) {
        memcpy(report->first, pass.first, sizeof(report->first));
        report->errors = pass.errors;
        report->unfixed = pass.errors;
    }

    mfs_unlock();

    return rc;
}

int mfs_mkdir(const char *path)
{
    int rc;

    mfs_lock();
    rc = mfs_mkdir_locked(path);
    mfs_unlock();

    return rc;
}

int mfs_create(const char *path)
{
    int rc;

    mfs_lock();
    rc = mfs_create_locked(path);
    mfs_unlock();

    return rc;
}

int mfs_open(const char *path, int flags, struct mfs_file *out)
{
    int rc;

    mfs_lock();
    rc = mfs_open_locked(path, flags, out);

    if (rc == MFS_OK) {
        g.open_files++;
    }

    mfs_unlock();

    return rc;
}

int mfs_read(struct mfs_file *file, void *buf, k_size_t size)
{
    int rc;

    mfs_lock();
    rc = mfs_read_locked(file, buf, size);
    mfs_unlock();

    return rc;
}

int mfs_write(struct mfs_file *file, const void *buf, k_size_t size)
{
    int rc;

    mfs_lock();
    rc = mfs_write_locked(file, buf, size);
    mfs_unlock();

    return rc;
}

int mfs_truncate(struct mfs_file *file, k_u64_t size)
{
    int rc;

    mfs_lock();
    rc = mfs_truncate_locked(file, size);
    mfs_unlock();

    return rc;
}

int mfs_close(struct mfs_file *file)
{
    int rc;

    mfs_lock();
    rc = mfs_close_locked(file);

    if (rc == MFS_OK && g.open_files > 0u) {
        g.open_files--;
    }

    mfs_unlock();

    return rc;
}

int mfs_unlink(const char *path)
{
    int rc;

    mfs_lock();
    rc = mfs_remove_locked(path, false);
    mfs_unlock();

    return rc;
}

int mfs_rmdir(const char *path)
{
    int rc;

    mfs_lock();
    rc = mfs_remove_locked(path, true);
    mfs_unlock();

    return rc;
}

int mfs_rename(const char *from, const char *to)
{
    int rc;

    mfs_lock();
    rc = mfs_rename_locked(from, to);
    mfs_unlock();

    return rc;
}

int mfs_stat(const char *path, struct mfs_inode *out, struct mfs_dirent *dirent)
{
    int rc;

    mfs_lock();
    rc = mfs_stat_locked(path, out, dirent);
    mfs_unlock();

    return rc;
}

int mfs_chmod(const char *path, k_u32_t mode)
{
    int rc;

    mfs_lock();
    rc = mfs_chmod_locked(path, mode);
    mfs_unlock();

    return rc;
}

int mfs_chown(const char *path, k_u32_t owner)
{
    int rc;

    mfs_lock();
    rc = mfs_chown_locked(path, owner);
    mfs_unlock();

    return rc;
}

int mfs_chgrp(const char *path, k_u32_t group)
{
    int rc;

    mfs_lock();
    rc = mfs_chgrp_locked(path, group);
    mfs_unlock();

    return rc;
}

int mfs_list(const char *path, struct mfs_dirent *out, k_size_t max)
{
    int rc;

    mfs_lock();
    rc = mfs_list_locked(path, out, max);
    mfs_unlock();

    return rc;
}

int mfs_readdir(struct mfs_file *file, struct mfs_dirent *out, k_size_t max, k_size_t *count)
{
    int rc;

    mfs_lock();
    rc = mfs_readdir_locked(file, out, max, count);
    mfs_unlock();

    return rc;
}

const char *mfs_strerror(int error)
{
    switch (error) {
    case MFS_OK:
        return "no error";
    case MFS_ENOENT:
        return "no such file or directory";
    case MFS_EEXIST:
        return "file exists";
    case MFS_ENOSPC:
        return "no space left";
    case MFS_EIO:
        return "input/output error";
    case MFS_EINVAL:
        return "invalid argument";
    case MFS_ENOTDIR:
        return "not a directory";
    case MFS_EISDIR:
        return "is a directory";
    case MFS_ENOTEMPTY:
        return "directory not empty";
    case MFS_E2BIG:
        return "file too big";
    case MFS_ENOTSUP:
        return "operation not supported";
    case MFS_ENOMEM:
        return "out of memory";
    default:
        return "unknown error";
    }
}
