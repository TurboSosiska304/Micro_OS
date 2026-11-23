/*
 * Micro OS — the write side of ext2: the bitmaps of a group, the inode of an
 * object, the records of a directory and the bytes of a file.
 *
 * The format is the one of ADR 0019 and it is not extended here: the
 * allocation takes a run of the free bits of a group from a cursor and keeps
 * the counters of the group and of the superblock true; an inode names its
 * blocks through the twelve direct pointers and the three tables of
 * pointers of the format, so a file of a large volume grows as far as the
 * format allows it (2 GiB of blocks of 1 KiB, 4 TiB of blocks of 8 KiB)
 * and not to the ceiling of one table.
 *
 * The group the writer is working in is one slot of the volume: its
 * descriptor and one of its bitmaps stay in the memory and reach the medium
 * when the volume settles, when the writer moves to another group and at the
 * end of a run of the medium (ADR 0037). A file is written in runs of the
 * blocks that the bitmap gives one after another, so a write of 64 KiB is a
 * handful of commands of the disk and not sixty-four.
 *
 * The order of every change is the order e2fsck reads: a block is marked in
 * the bitmap before an inode names it, an inode is marked before a
 * directory names it, and the counters of the superblock are written when
 * the volume leaves. A volume that was only read is left untouched
 * (ext2_volume_flush writes nothing when the volume is not dirty).
 */
#include "ext2_private.h"

#include <string.h>

#include "kernel/clock.h"
#include "kernel/errno.h"
#include "kernel/log.h"

/* The moment of the wall clock in the seconds of the medium. A zero marks
 * an inode that was never used, so a machine whose uptime is under a second
 * stamps one rather than lying about it. */
k_u32_t ext2_now(void)
{
    k_u64_t now = k_clock_now_unix();

    if (now == 0u) {
        return 1u;
    }

    if (now > 0xFFFFFFFFu) {
        return 0xFFFFFFFFu;
    }

    return (k_u32_t)now;
}

static void inode_new(struct ext2_inode *inode, k_u32_t number, k_u16_t mode, k_u16_t links)
{
    memset(inode, 0, sizeof(*inode));
    inode->number = number;
    inode->mode = mode;
    inode->uid = 0; /* the one user of the system, ADR 0005 */
    inode->gid = 0;
    inode->links = links;
    inode->atime = ext2_now();
    inode->ctime = inode->atime;
    inode->mtime = inode->atime;
}

/* The group that holds a block of the volume. */
static k_u32_t block_group(const struct ext2_volume *vol, k_u32_t block)
{
    if (block <= vol->first_data_block) {
        return 0;
    }

    return (block - vol->first_data_block) / vol->blocks_per_group;
}

/* The first block of a group. */
static k_u32_t group_first_block(const struct ext2_volume *vol, k_u32_t group)
{
    return vol->first_data_block + group * vol->blocks_per_group;
}

/* The index of an inode inside its group. */
static k_u32_t inode_index(const struct ext2_volume *vol, k_u32_t number, k_u32_t *group)
{
    *group = (number - 1u) / vol->inodes_per_group;
    return (number - 1u) % vol->inodes_per_group;
}

/* The sector count of one block: the unit of i_blocks. */
static k_u32_t block_sectors(const struct ext2_volume *vol)
{
    return vol->block_size / 512u;
}

int ext2_inode_write(struct ext2_volume *vol, const struct ext2_inode *inode, bool preserve)
{
    struct ext2_group group;
    k_u8_t raw[EXT2_INODE_MIN_SIZE];
    k_u32_t index;
    k_u32_t number;
    k_u32_t group_index;
    k_u32_t inside;
    k_u64_t offset;
    k_u32_t block;
    k_u32_t i;
    int rc;

    if (vol == NULL || inode == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    number = inode->number;
    if (number == 0u || number > vol->inodes_count) {
        return KERR_IO;
    }

    index = inode_index(vol, number, &group_index);
    rc = ext2_group_read(vol, group_index, &group);
    if (rc != 0) {
        return rc;
    }

    offset = (k_u64_t)index * vol->inode_size;
    block = group.inode_table + (k_u32_t)(offset / vol->block_size);
    inside = (k_u32_t)(offset % vol->block_size);

    if (block >= vol->blocks_count || inside + vol->inode_size > vol->block_size) {
        return KERR_IO;
    }

    rc = ext2_read_block(vol, block, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    if (!preserve) {
        /* A fresh inode: every field that this writer does not name - an
         * ACL, a fragment, the extra fields of a longer record - is zero.
         * The slot may be one a removed file used, so the tail counts. */
        memset(vol->block_buf + inside, 0, vol->inode_size);
    }

    memcpy(raw, vol->block_buf + inside, sizeof(raw));

    ext2_put16(raw + 0, inode->mode);
    ext2_put16(raw + 2, inode->uid);
    ext2_put32(raw + 4, (k_u32_t)inode->size);
    ext2_put32(raw + 8, inode->atime);
    ext2_put32(raw + 12, inode->ctime);
    ext2_put32(raw + 16, inode->mtime);
    ext2_put32(raw + 20, inode->dtime);
    ext2_put16(raw + 24, inode->gid);
    ext2_put16(raw + 26, inode->links);
    ext2_put32(raw + 28, (k_u32_t)inode->blocks);
    ext2_put32(raw + 32, inode->flags);

    for (i = 0; i < 15u; i++) {
        ext2_put32(raw + 40u + i * 4u, inode->block[i]);
    }

    if ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
        /* The high half of the size of a file lives where an older layout
         * kept the ACL of a directory. */
        ext2_put32(raw + 108u, (k_u32_t)(inode->size >> 32));
    }

    memcpy(vol->block_buf + inside, raw, sizeof(raw));
    vol->dirty = true;

    return ext2_write_block(vol, block, vol->block_buf);
}

/* --- the state of the group the writer is working in ---------------------- */

/*
 * The bitmap of the group of the slot, loaded there, and the one of the two
 * bitmaps the writer asked for. The bitmap of a group is a block of the
 * volume: a writer that reads it, sets a bit and writes it again for every
 * block of a file pays two commands of the medium for a kilobyte of data.
 * The slot keeps it until another group or the other bitmap is needed, and
 * `ext2_write_flush` is where it reaches the medium.
 */
static int bitmap_load(struct ext2_volume *vol, k_u32_t group, k_u8_t kind)
{
    struct ext2_group loaded;
    k_u32_t block;
    int rc;

    rc = ext2_group_read(vol, group, &loaded);
    if (rc != 0) {
        return rc;
    }

    if (vol->bitmap_kind == kind) {
        return 0;
    }

    rc = ext2_write_flush(vol);
    if (rc != 0) {
        return rc;
    }

    block = (kind == EXT2_BITMAP_INODE) ? loaded.inode_bitmap : loaded.block_bitmap;

    rc = ext2_read_block(vol, block, vol->bitmap);
    if (rc != 0) {
        vol->bitmap_kind = EXT2_BITMAP_NONE;
        return rc;
    }

    vol->bitmap_kind = kind;
    vol->bitmap_dirty = false;

    return 0;
}

/* The first free bit of a bitmap at or after `from`, wrapping to the first
 * bit, over the `bits` that name a block or an inode of the volume. A byte
 * that is all ones is skipped as a byte and not as eight bits, which is what
 * makes the walk of a full group of 8192 bits cheap. `bits` is the answer of
 * "no free bit". */
static k_u32_t bitmap_scan(const k_u8_t *map, k_u32_t bits, k_u32_t from)
{
    k_u32_t start = (from < bits) ? from : 0u;
    k_u32_t pass;

    for (pass = 0; pass < 2u; pass++) {
        k_u32_t i = (pass == 0u) ? start : 0u;
        k_u32_t limit = (pass == 0u) ? bits : start;

        while (i < limit) {
            k_u8_t byte = map[i >> 3];
            k_u32_t end;
            k_u32_t j;

            if (byte == 0xFFu) {
                i = (i | 7u) + 1u;
                continue;
            }

            end = (i | 7u) + 1u;
            if (end > limit) {
                end = limit;
            }

            for (j = i; j < end; j++) {
                if ((byte & (k_u8_t)(1u << (j & 7u))) == 0u) {
                    return j;
                }
            }

            i = end;
        }
    }

    return bits;
}

static bool bitmap_used(const k_u8_t *map, k_u32_t bit)
{
    return (map[bit >> 3] & (k_u8_t)(1u << (bit & 7u))) != 0u;
}

static void bitmap_set(k_u8_t *map, k_u32_t bit, bool used)
{
    if (used) {
        map[bit >> 3] |= (k_u8_t)(1u << (bit & 7u));
    } else {
        map[bit >> 3] &= (k_u8_t) ~(1u << (bit & 7u));
    }
}

/* The most blocks one allocation asks for: a run the medium moves in one
 * command, and never more than the blocks of a table of pointers. */
static k_u32_t run_blocks(const struct ext2_volume *vol)
{
    k_u32_t blocks = EXT2_IO_BYTES_MAX / vol->block_size;

    return (blocks == 0u) ? 1u : blocks;
}

/* The blocks of a group that the volume has, from its first one. */
static k_u32_t group_bits(const struct ext2_volume *vol, k_u32_t first)
{
    k_u32_t bits = vol->blocks_per_group;

    if (first >= vol->blocks_count) {
        return 0;
    }

    if (bits > vol->blocks_count - first) {
        bits = vol->blocks_count - first;
    }

    return bits;
}

/*
 * Take a run of the blocks of a group: the `want` bits from the cursor on
 * that are free and follow one another, at most to the end of the group. The
 * scan starts where the last allocation stopped, so the blocks of a file are
 * the blocks of the group in order and not the first hole of the volume for
 * every block; a group that has no room answers KERR_NOSPC and the caller
 * looks at the next one.
 */
static int group_alloc_run(struct ext2_volume *vol, struct ext2_group *group, k_u32_t want,
                           k_u32_t *first, k_u32_t *count)
{
    k_u32_t base = group_first_block(vol, group->number);
    k_u32_t bits = group_bits(vol, base);
    k_u32_t from = 0;
    k_u32_t bit;
    k_u32_t n;
    k_u32_t i;
    int rc;

    *first = 0;
    *count = 0;

    if (bits == 0u) {
        return KERR_NOSPC;
    }

    if (run_blocks(vol) < want) {
        want = run_blocks(vol);
    }

    rc = bitmap_load(vol, group->number, EXT2_BITMAP_BLOCK);
    if (rc != 0) {
        return rc;
    }

    if (vol->alloc_hint == group->number && vol->alloc_cursor < bits) {
        from = vol->alloc_cursor;
    }

    bit = bitmap_scan(vol->bitmap, bits, from);

    if (bit == bits) {
        return KERR_NOSPC;
    }

    /* The run is as long as the free bits that follow the first one. */
    n = 1;
    while (n < want && bit + n < bits && !bitmap_used(vol->bitmap, bit + n)) {
        n++;
    }

    for (i = 0; i < n; i++) {
        bitmap_set(vol->bitmap, bit + i, true);
    }

    vol->bitmap_dirty = true;
    vol->alloc_hint = group->number;
    vol->alloc_cursor = bit + n;

    if (vol->alloc_cursor >= bits) {
        vol->alloc_cursor = 0;
    }

    if (group->free_blocks >= n) {
        group->free_blocks = (k_u16_t)(group->free_blocks - n);
    } else {
        group->free_blocks = 0;
    }

    rc = ext2_group_write(vol, group);
    if (rc != 0) {
        return rc;
    }

    if (vol->free_blocks >= n) {
        vol->free_blocks -= n;
    } else {
        vol->free_blocks = 0;
    }

    vol->dirty = true;
    *first = base + bit;
    *count = n;

    return 0;
}

int ext2_alloc_blocks(struct ext2_volume *vol, k_u32_t want, k_u32_t *block, k_u32_t *count)
{
    k_u32_t start;
    k_u32_t tried;

    if (vol == NULL || block == NULL || count == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    *block = 0;
    *count = 0;

    if (want == 0u) {
        return 0;
    }

    if (vol->groups == 0u) {
        return KERR_NOSPC;
    }

    start = vol->alloc_hint % vol->groups;

    for (tried = 0; tried < vol->groups; tried++) {
        k_u32_t index = (start + tried) % vol->groups;
        struct ext2_group group;
        int rc;

        rc = ext2_group_read(vol, index, &group);
        if (rc != 0) {
            return rc;
        }

        if (group.free_blocks == 0u) {
            continue;
        }

        rc = group_alloc_run(vol, &group, want, block, count);
        if (rc == KERR_NOSPC) {
            continue;
        }
        if (rc != 0) {
            return rc;
        }

        return 0;
    }

    return KERR_NOSPC;
}

int ext2_alloc_block(struct ext2_volume *vol, k_u32_t *block)
{
    k_u32_t count = 0;
    int rc = ext2_alloc_blocks(vol, 1, block, &count);

    if (rc == 0 && count != 1u) {
        return KERR_IO;
    }

    return rc;
}

int ext2_free_block(struct ext2_volume *vol, k_u32_t block)
{
    struct ext2_group group;
    k_u32_t index;
    k_u32_t first;
    k_u32_t bit;
    k_u32_t bits;
    int rc;

    if (vol == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (block < vol->first_data_block || block >= vol->blocks_count) {
        return KERR_IO;
    }

    index = block_group(vol, block);
    rc = ext2_group_read(vol, index, &group);
    if (rc != 0) {
        return rc;
    }

    first = group_first_block(vol, index);
    bits = group_bits(vol, first);
    bit = block - first;

    if (bit >= bits) {
        return KERR_IO;
    }

    rc = bitmap_load(vol, index, EXT2_BITMAP_BLOCK);
    if (rc != 0) {
        return rc;
    }

    if (!bitmap_used(vol->bitmap, bit)) {
        return KERR_IO; /* the block was already free */
    }

    bitmap_set(vol->bitmap, bit, false);
    vol->bitmap_dirty = true;

    /* The room of a removal is the room of the next object: the cursor of
     * the allocation goes back to the block that was just given. */
    vol->alloc_hint = index;
    vol->alloc_cursor = bit;

    group.free_blocks++;
    rc = ext2_group_write(vol, &group);
    if (rc != 0) {
        return rc;
    }

    vol->free_blocks++;
    vol->dirty = true;

    return 0;
}

/* Take an inode from the bitmap of a group. */
static int group_alloc_inode(struct ext2_volume *vol, struct ext2_group *group, k_u32_t *number)
{
    k_u32_t first = group->number * vol->inodes_per_group;
    k_u32_t bits = vol->inodes_per_group;
    k_u32_t from = 0;
    k_u32_t bit;
    int rc;

    *number = 0;

    if (first >= vol->inodes_count) {
        return KERR_NOSPC;
    }

    if (bits > vol->inodes_count - first) {
        bits = vol->inodes_count - first;
    }

    rc = bitmap_load(vol, group->number, EXT2_BITMAP_INODE);
    if (rc != 0) {
        return rc;
    }

    if (vol->inode_hint == group->number && vol->inode_cursor < bits) {
        from = vol->inode_cursor;
    }

    bit = bitmap_scan(vol->bitmap, bits, from);

    if (bit == bits) {
        return KERR_NOSPC;
    }

    bitmap_set(vol->bitmap, bit, true);
    vol->bitmap_dirty = true;

    vol->inode_hint = group->number;
    vol->inode_cursor = bit + 1u;

    if (vol->inode_cursor >= bits) {
        vol->inode_cursor = 0;
    }

    if (group->free_inodes > 0u) {
        group->free_inodes--;
    }

    rc = ext2_group_write(vol, group);
    if (rc != 0) {
        return rc;
    }

    *number = first + bit + 1u;

    if (vol->free_inodes > 0u) {
        vol->free_inodes--;
    }

    vol->dirty = true;

    return 0;
}

int ext2_alloc_inode(struct ext2_volume *vol, k_u32_t *number)
{
    k_u32_t start;
    k_u32_t tried;

    if (vol == NULL || number == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (vol->groups == 0u) {
        return KERR_NOSPC;
    }

    start = vol->inode_hint % vol->groups;

    for (tried = 0; tried < vol->groups; tried++) {
        k_u32_t index = (start + tried) % vol->groups;
        struct ext2_group group;
        int rc;

        rc = ext2_group_read(vol, index, &group);
        if (rc != 0) {
            return rc;
        }

        if (group.free_inodes == 0u) {
            continue;
        }

        rc = group_alloc_inode(vol, &group, number);
        if (rc == KERR_NOSPC) {
            continue;
        }
        if (rc != 0) {
            return rc;
        }

        vol->dirty = true;

        return 0;
    }

    return KERR_NOSPC;
}

int ext2_free_inode(struct ext2_volume *vol, k_u32_t number)
{
    struct ext2_group group;
    k_u32_t index;
    k_u32_t group_index;
    int rc;

    if (vol == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (number == 0u || number > vol->inodes_count) {
        return KERR_IO;
    }

    index = inode_index(vol, number, &group_index);
    rc = ext2_group_read(vol, group_index, &group);
    if (rc != 0) {
        return rc;
    }

    rc = bitmap_load(vol, group_index, EXT2_BITMAP_INODE);
    if (rc != 0) {
        return rc;
    }

    if (!bitmap_used(vol->bitmap, index)) {
        return KERR_IO;
    }

    bitmap_set(vol->bitmap, index, false);
    vol->bitmap_dirty = true;

    vol->inode_hint = group_index;
    vol->inode_cursor = index;

    group.free_inodes++;
    rc = ext2_group_write(vol, &group);
    if (rc != 0) {
        return rc;
    }

    vol->free_inodes++;
    vol->dirty = true;

    return 0;
}

/*
 * The table that names the entries of `level` for the logical block `index`,
 * and the entry of `index` in it. `slot` is the pointer in the inode or in
 * the table above, and `level` is how many tables are still under the entry:
 * one is the table of the single indirection, whose entries name data
 * blocks, two and three are the tables of the double and the triple
 * indirection, whose entries name the table below. `index` is the position in
 * the whole subtree of that table, so the entry of this level is
 * `index / stride` and the part below it `index % stride`.
 *
 * A table of zero is a hole: a read of the suffix of a file that was never
 * written answers zeros. With `create` the tables on the way are made and
 * zeroed - every entry of a table names a block, and a table whose other
 * entries are the bytes of an object that is gone is a volume e2fsck calls
 * broken - and the entry of the parent is written back, because a table the
 * parent does not name is a table the volume cannot reach.
 *
 * `table` comes back zero when a table on the way is not there and `create`
 * is not set; `entry` is the index of the entry of `index` in the leaf table.
 */
static int table_walk(struct ext2_volume *vol, struct ext2_inode *inode, k_u32_t *slot,
                      unsigned level, k_u64_t index, bool create, k_u32_t *table, k_u64_t *entry)
{
    k_u32_t pointers = vol->block_size / 4u;
    k_u64_t stride = 1;
    k_u64_t offset;
    k_u8_t raw[4];
    k_u32_t child = *slot;
    unsigned i;
    int rc;

    *table = 0;
    *entry = 0;

    for (i = 1; i < level; i++) {
        stride *= pointers;
    }

    if (level == 0u || level > 3u || index >= stride * pointers) {
        return KERR_IO;
    }

    if (child == 0u) {
        if (!create) {
            return 0;
        }

        rc = ext2_alloc_block(vol, &child);
        if (rc != 0) {
            return rc;
        }

        /* The bytes of the entries of a new table are zero: an entry of a
         * table of pointers names a block of the volume, and the blocks a
         * file does not reach are the holes it reads as zeros. */
        memset(vol->block_buf, 0, vol->block_size);
        rc = ext2_write_block(vol, child, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        inode->blocks += block_sectors(vol);
        *slot = child;
    }

    if (child >= vol->blocks_count) {
        return KERR_IO;
    }

    if (level == 1u) {
        *table = child;
        *entry = index;
        return 0;
    }

    offset = (k_u64_t)child * vol->block_size + (index / stride) * 4u;

    rc = ext2_read_bytes(vol, offset, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    {
        k_u32_t below = ext2_le32(raw);

        if (below >= vol->blocks_count) {
            return KERR_IO;
        }

        rc = table_walk(vol, inode, &below, level - 1u, index % stride, create, table, entry);
        if (rc != 0) {
            return rc;
        }

        /* The table below was there, or this call made none: the parent
         * keeps the entry it had. */
        if (below == ext2_le32(raw)) {
            return 0;
        }

        ext2_put32(raw, below);

        return ext2_write_bytes(vol, offset, raw, sizeof(raw));
    }
}

/*
 * One entry of a table of pointers, read or made: the block that holds the
 * logical block `index` of the table of `level`, allocated when `create` is
 * set and the entry is a hole. `fresh` is true for the block this call gave.
 */
static int table_entry(struct ext2_volume *vol, struct ext2_inode *inode, k_u32_t *slot,
                       unsigned level, k_u64_t index, bool create, k_u32_t *out, bool *fresh)
{
    k_u32_t table = 0;
    k_u64_t entry = 0;
    k_u64_t offset;
    k_u8_t raw[4];
    k_u32_t value;
    int rc;

    *out = 0;
    if (fresh != NULL) {
        *fresh = false;
    }

    rc = table_walk(vol, inode, slot, level, index, create, &table, &entry);
    if (rc != 0 || table == 0u) {
        return rc;
    }

    offset = (k_u64_t)table * vol->block_size + entry * 4u;

    rc = ext2_read_bytes(vol, offset, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    value = ext2_le32(raw);

    if (value >= vol->blocks_count) {
        return KERR_IO;
    }

    if (value == 0u) {
        if (!create) {
            return 0;
        }

        rc = ext2_alloc_block(vol, &value);
        if (rc != 0) {
            return rc;
        }

        inode->blocks += block_sectors(vol);
        ext2_put32(raw, value);

        rc = ext2_write_bytes(vol, offset, raw, sizeof(raw));
        if (rc != 0) {
            return rc;
        }

        if (fresh != NULL) {
            *fresh = true;
        }
    }

    *out = value;

    return 0;
}

int ext2_inode_get_block(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t logical,
                         k_u32_t *block, bool create, bool *fresh)
{
    k_u64_t pointers;
    k_u64_t double_start;
    k_u64_t triple_start;

    if (vol == NULL || inode == NULL || block == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    pointers = vol->block_size / 4u;
    double_start = EXT2_DIRECT_BLOCKS + pointers;
    triple_start = double_start + pointers * pointers;
    *block = 0;

    if (fresh != NULL) {
        *fresh = false;
    }

    if (logical < EXT2_DIRECT_BLOCKS) {
        k_u32_t value = inode->block[logical];
        int rc;

        if (value == 0u && create) {
            rc = ext2_alloc_block(vol, &value);
            if (rc != 0) {
                return rc;
            }

            inode->block[logical] = value;
            inode->blocks += block_sectors(vol);

            if (fresh != NULL) {
                *fresh = true;
            }
        }

        if (value == 0u) {
            return 0;
        }

        if (value >= vol->blocks_count) {
            return KERR_IO;
        }

        *block = value;
        return 0;
    }

    if (logical < double_start) {
        return table_entry(vol, inode, &inode->block[12], 1u, logical - EXT2_DIRECT_BLOCKS, create,
                           block, fresh);
    }

    if (logical < triple_start) {
        return table_entry(vol, inode, &inode->block[13], 2u, logical - double_start, create, block,
                           fresh);
    }

    return table_entry(vol, inode, &inode->block[14], 3u, logical - triple_start, create, block,
                       fresh);
}

/*
 * Name a run of blocks of a file: the entries of `count` logical blocks from
 * `logical` on become the blocks of the volume that follow `first`. The
 * entries of a run live in one table of pointers, so the table is read once,
 * changed and written once, and a file of the direct blocks names its run in
 * the inode itself.
 */
static int inode_store_run(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t logical,
                           k_u32_t first, k_u32_t count)
{
    k_u64_t pointers = vol->block_size / 4u;
    k_u64_t double_start = EXT2_DIRECT_BLOCKS + pointers;
    k_u64_t triple_start = double_start + pointers * pointers;
    k_u32_t table = 0;
    k_u64_t entry = 0;
    k_u32_t i;
    int rc;

    if (logical < EXT2_DIRECT_BLOCKS) {
        for (i = 0; i < count; i++) {
            inode->block[logical + i] = first + i;
        }
    } else {
        k_u64_t index;
        unsigned level;
        k_u64_t offset;

        if (logical < double_start) {
            index = logical - EXT2_DIRECT_BLOCKS;
            level = 1u;
            rc = table_walk(vol, inode, &inode->block[12], level, index, true, &table, &entry);
        } else if (logical < triple_start) {
            index = logical - double_start;
            level = 2u;
            rc = table_walk(vol, inode, &inode->block[13], level, index, true, &table, &entry);
        } else {
            index = logical - triple_start;
            level = 3u;
            rc = table_walk(vol, inode, &inode->block[14], level, index, true, &table, &entry);
        }

        if (rc != 0) {
            return rc;
        }

        /* The entries of the run are one part of the table: read it, name
         * the blocks, write it back. The table of a run that was just made
         * is zeros, and the read is a read of the cache. */
        offset = (k_u64_t)(entry + count) * 4u;

        if (table == 0u || offset > vol->block_size) {
            return KERR_IO;
        }

        rc = ext2_read_block(vol, table, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        for (i = 0; i < count; i++) {
            ext2_put32(vol->block_buf + (k_u32_t)(entry + i) * 4u, first + i);
        }

        rc = ext2_write_block(vol, table, vol->block_buf);
        if (rc != 0) {
            return rc;
        }
    }

    inode->blocks += (k_u64_t)count * block_sectors(vol);

    return 0;
}

/*
 * The whole blocks of a write that begins at `logical`, as one run of the
 * medium: the blocks of the run are allocated together when they are the
 * holes of the end of the file, and the blocks the file already holds are
 * taken together when they follow one another in the volume. `first` is the
 * block of the first one, `count` the blocks of the run, and `fresh` is true
 * when the run was just allocated and holds no bytes of anybody.
 *
 * A run stops at the table that names its blocks, at an entry the file
 * already has, and at the room the bitmap can give: a write that finds a
 * block in the middle of the run falls back to a block at a time, which is
 * what a file written out of order costs.
 */
static int file_run(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t logical,
                    k_u32_t want, k_u32_t *first, k_u32_t *count, bool *fresh)
{
    k_u32_t base = 0;
    k_u32_t n = 0;
    k_u32_t i;
    int rc;

    *first = 0;
    *count = 0;
    *fresh = false;

    rc = ext2_block_run(vol, inode, logical, want, &base, &n);
    if (rc != 0) {
        return rc;
    }

    if (n == 0u) {
        return KERR_IO;
    }

    if (base != 0u) {
        *first = base;
        *count = n;
        return 0;
    }

    {
        k_u32_t block = 0;
        k_u32_t got = 0;

        rc = ext2_alloc_blocks(vol, n, &block, &got);
        if (rc != 0) {
            return rc;
        }

        if (got == 0u) {
            return KERR_NOSPC;
        }

        rc = inode_store_run(vol, inode, logical, block, got);
        if (rc != 0) {
            for (i = 0; i < got; i++) {
                (void)ext2_free_block(vol, block + i);
            }
            return rc;
        }

        *first = block;
        *count = got;
        *fresh = true;
    }

    return 0;
}

int ext2_file_write(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t offset,
                    const void *buf, k_size_t size, k_size_t *got)
{
    const k_u8_t *src = (const k_u8_t *)buf;
    k_size_t done = 0;
    int rc = 0;

    if (vol == NULL || inode == NULL || got == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    *got = 0;

    if (ext2_inode_is_dir(inode)) {
        return KERR_ISDIR;
    }

    if (!ext2_inode_is_file(inode)) {
        return KERR_NOTSUP;
    }

    if (size == 0) {
        return 0;
    }

    while (done < size) {
        k_u64_t position = offset + done;
        k_u64_t logical = position / vol->block_size;
        k_u32_t inside = (k_u32_t)(position % vol->block_size);
        k_u32_t block = 0;
        bool fresh = false;

        if (inside == 0u && size - done >= vol->block_size) {
            /* A run of whole blocks: the blocks of the file and the bytes
             * of the call leave together, which is one command of the
             * medium for the 64 KiB a bus master moves at once instead of
             * one for the kilobyte of every block (ADR 0037). */
            k_u32_t want = (k_u32_t)((size - done) / vol->block_size);
            k_u32_t first = 0;
            k_u32_t count = 0;

            rc = file_run(vol, inode, logical, want, &first, &count, &fresh);
            if (rc != 0) {
                break;
            }

            if (fresh) {
                /* The bit of a block that names it reaches the medium before
                 * the bytes of the block do: a machine that stops between
                 * the two leaves blocks that the map calls free, never a
                 * block that a file names and the map calls free. */
                rc = ext2_write_flush(vol);
            }

            if (rc == 0) {
                rc = ext2_write_bytes(vol, (k_u64_t)first * vol->block_size, src + done,
                                      (k_size_t)count * vol->block_size);
            }

            if (rc != 0) {
                break;
            }

            done += (k_size_t)count * vol->block_size;
            continue;
        }

        /* The part of a call that is not a whole block: the tail of a block
         * and the stub of a file of a few bytes. */
        {
            k_size_t chunk = vol->block_size - inside;

            if (chunk > size - done) {
                chunk = size - done;
            }

            rc = ext2_inode_get_block(vol, inode, logical, &block, true, &fresh);
            if (rc != 0) {
                break;
            }

            if (block == 0u) {
                rc = KERR_IO;
                break;
            }

            if (inside == 0u && chunk == vol->block_size) {
                if (fresh) {
                    rc = ext2_write_flush(vol);
                }

                if (rc == 0) {
                    rc = ext2_write_bytes(vol, (k_u64_t)block * vol->block_size, src + done, chunk);
                }
            } else {
                if (fresh) {
                    /* A block this call allocated holds nobody's bytes: the
                     * part of it that the call does not write is zeros, and
                     * a hole of the file reads as zeros because it is a hole
                     * and not because of what an object that is gone left
                     * behind. */
                    memset(vol->block_buf, 0, vol->block_size);
                    rc = 0;
                } else {
                    rc = ext2_read_block(vol, block, vol->block_buf);
                }

                if (rc == 0) {
                    memcpy(vol->block_buf + inside, src + done, chunk);
                    rc = ext2_write_flush(vol);
                }

                if (rc == 0) {
                    rc = ext2_write_block(vol, block, vol->block_buf);
                }
            }

            if (rc != 0) {
                break;
            }

            done += chunk;
        }
    }

    if (done > 0) {
        if (offset + done > inode->size) {
            inode->size = offset + done;
        }

        inode->mtime = ext2_now();
        inode->ctime = inode->mtime;

        {
            int written = ext2_inode_write(vol, inode, true);

            if (rc == 0) {
                rc = written;
            }
        }
    }

    *got = done;

    return rc;
}

/*
 * Give back the blocks of a table of pointers from the entry `keep` on: the
 * entries in front of it keep their subtrees, the entry that holds the
 * boundary is cut at its own boundary, and the rest is freed whole. A table
 * that keeps nothing is freed itself.
 */
static int free_table_keep(struct ext2_volume *vol, k_u32_t table, unsigned level, k_u64_t keep,
                           k_u32_t *out_table, k_u32_t *freed_sectors)
{
    k_u32_t pointers = vol->block_size / 4u;
    k_u64_t stride;
    k_u64_t full;
    k_u64_t rest;
    k_u32_t i;
    int rc;

    *out_table = table;

    if (table == 0u) {
        return 0;
    }

    if (table >= vol->blocks_count) {
        return KERR_IO;
    }

    stride = 1;
    for (i = 1; i < level; i++) {
        stride *= pointers;
    }

    if (keep >= (k_u64_t)pointers * stride) {
        return 0; /* every entry of this table is in front of the cut */
    }

    full = keep / stride; /* the entries that stay whole */
    rest = keep % stride; /* the part of the next subtree that stays */

    for (i = pointers; i > full; i--) {
        k_u32_t entry_index = i - 1u;
        k_u64_t offset = (k_u64_t)table * vol->block_size + (k_u64_t)entry_index * 4u;
        k_u8_t raw[4];
        k_u32_t child;

        rc = ext2_read_bytes(vol, offset, raw, sizeof(raw));
        if (rc != 0) {
            return rc;
        }

        child = ext2_le32(raw);
        if (child == 0u) {
            continue;
        }

        if (level == 1u) {
            rc = ext2_free_block(vol, child);
            if (rc != 0) {
                return rc;
            }

            *freed_sectors += block_sectors(vol);
        } else {
            k_u32_t kept = 0;
            k_u64_t below = 0;

            if (entry_index == full && rest != 0u) {
                below = rest;
            }

            rc = free_table_keep(vol, child, level - 1u, below, &kept, freed_sectors);
            if (rc != 0) {
                return rc;
            }

            if (kept != 0u) {
                /* The subtree is partly kept: its number stays. */
                continue;
            }
        }

        ext2_put32(raw, 0);
        rc = ext2_write_bytes(vol, offset, raw, sizeof(raw));
        if (rc != 0) {
            return rc;
        }
    }

    if (keep == 0u) {
        rc = ext2_free_block(vol, table);
        if (rc != 0) {
            return rc;
        }

        *freed_sectors += block_sectors(vol);
        *out_table = 0;
    }

    return 0;
}

/* Give back every block of an inode from logical `keep` on. */
static int free_range(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t keep,
                      k_u32_t *freed_sectors)
{
    k_u32_t pointers = vol->block_size / 4u;
    k_u64_t single = EXT2_DIRECT_BLOCKS;
    k_u64_t double_start = single + pointers;
    k_u64_t triple_start = double_start + (k_u64_t)pointers * pointers;
    k_u64_t triple_end = triple_start + (k_u64_t)pointers * pointers * pointers;
    k_u64_t below;
    k_u32_t i;
    int rc;

    for (i = EXT2_DIRECT_BLOCKS; i > 0u; i--) {
        k_u32_t index = i - 1u;

        if ((k_u64_t)index < keep) {
            break;
        }

        if (inode->block[index] != 0u) {
            rc = ext2_free_block(vol, inode->block[index]);
            if (rc != 0) {
                return rc;
            }

            *freed_sectors += block_sectors(vol);
            inode->block[index] = 0;
        }
    }

    below = (keep > single) ? keep - single : 0u;
    rc = free_table_keep(vol, inode->block[12], 1, below, &inode->block[12], freed_sectors);
    if (rc != 0) {
        return rc;
    }

    if (keep > double_start) {
        below = keep - double_start;
    } else {
        below = 0;
    }
    rc = free_table_keep(vol, inode->block[13], 2, below, &inode->block[13], freed_sectors);
    if (rc != 0) {
        return rc;
    }

    if (keep > triple_start) {
        below = keep - triple_start;
    } else {
        below = 0;
    }
    if (below >= triple_end - triple_start) {
        below = triple_end - triple_start;
    }
    return free_table_keep(vol, inode->block[14], 3, below, &inode->block[14], freed_sectors);
}

int ext2_truncate_inode(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t size)
{
    k_u32_t freed = 0;
    int rc;

    if (vol == NULL || inode == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (ext2_inode_is_dir(inode)) {
        return KERR_ISDIR;
    }

    if (!ext2_inode_is_file(inode)) {
        return KERR_NOTSUP;
    }

    if (size >= inode->size) {
        if (size > inode->size) {
            inode->size = size;
            inode->mtime = ext2_now();
            inode->ctime = inode->mtime;
            return ext2_inode_write(vol, inode, true);
        }

        return 0;
    }

    /* The tail of the block that stays: the bytes past the new size are
     * not the bytes of this file any more. */
    if (size > 0u && (size % vol->block_size) != 0u) {
        k_u32_t block = 0;

        rc = ext2_inode_get_block(vol, inode, size / vol->block_size, &block, false, NULL);
        if (rc != 0) {
            return rc;
        }

        if (block != 0u) {
            k_u32_t inside = (k_u32_t)(size % vol->block_size);

            rc = ext2_read_block(vol, block, vol->block_buf);
            if (rc != 0) {
                return rc;
            }

            memset(vol->block_buf + inside, 0, vol->block_size - inside);

            rc = ext2_write_block(vol, block, vol->block_buf);
            if (rc != 0) {
                return rc;
            }
        }
    }

    rc = free_range(vol, inode, (size + vol->block_size - 1u) / vol->block_size, &freed);
    if (rc != 0) {
        return rc;
    }

    inode->blocks = (inode->blocks > freed) ? inode->blocks - freed : 0u;
    inode->size = size;
    inode->mtime = ext2_now();
    inode->ctime = inode->mtime;

    return ext2_inode_write(vol, inode, true);
}

int ext2_split_parent(const char *path, char *parent, k_size_t parent_size, char *name,
                      k_size_t name_size)
{
    const char *slash = NULL;
    k_size_t length;
    k_size_t end;
    k_size_t parent_len;
    k_size_t name_len;
    k_size_t i;

    if (path == NULL || parent == NULL || name == NULL) {
        return KERR_INVAL;
    }

    length = strlen(path);
    if (length == 0u || path[0] != '/') {
        return KERR_INVAL;
    }

    /* A slash at the end names the same object; the name is the part after
     * the last slash that is not one of them. */
    end = length;
    while (end > 1u && path[end - 1u] == '/') {
        end--;
    }

    if (end == 1u) {
        return KERR_INVAL; /* the root has no name to change */
    }

    for (i = end; i > 0u; i--) {
        if (path[i - 1u] == '/') {
            slash = path + i - 1u;
            break;
        }
    }

    if (slash == NULL) {
        return KERR_INVAL;
    }

    parent_len = (k_size_t)(slash - path);
    name_len = end - parent_len - 1u;

    if (name_len == 0u || name_len > EXT2_NAME_MAX) {
        return KERR_NAMETOOLONG;
    }

    if (parent_len == 0u) {
        if (parent_size < 2u) {
            return KERR_NAMETOOLONG;
        }

        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (parent_len + 1u > parent_size) {
            return KERR_NAMETOOLONG;
        }

        memcpy(parent, path, parent_len);
        parent[parent_len] = '\0';
    }

    if (name_len + 1u > name_size) {
        return KERR_NAMETOOLONG;
    }

    memcpy(name, slash + 1, name_len);
    name[name_len] = '\0';

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return KERR_INVAL;
    }

    return 0;
}

/* The parent of a name, which has to be a directory of the volume. */
static int lookup_parent(struct ext2_volume *vol, const char *path, struct ext2_inode *parent,
                         char *name)
{
    char parent_path[VFS_PATH_MAX];
    char last[EXT2_NAME_MAX + 1];
    int rc;

    rc = ext2_split_parent(path, parent_path, sizeof(parent_path), last, sizeof(last));
    if (rc != 0) {
        return rc;
    }

    rc = ext2_lookup(vol, parent_path, parent, NULL);
    if (rc != 0) {
        return rc;
    }

    if (!ext2_inode_is_dir(parent)) {
        return KERR_NOTDIR;
    }

    memcpy(name, last, strlen(last) + 1u);

    return 0;
}

/* One more directory in the group of its inode: e2fsck counts them. */
static int group_add_dir(struct ext2_volume *vol, k_u32_t number, int change)
{
    struct ext2_group group;
    k_u32_t group_index;
    int rc;

    (void)inode_index(vol, number, &group_index);

    rc = ext2_group_read(vol, group_index, &group);
    if (rc != 0) {
        return rc;
    }

    if (change > 0) {
        group.used_dirs++;
    } else if (group.used_dirs > 0u) {
        group.used_dirs--;
    }

    return ext2_group_write(vol, &group);
}

int ext2_create_file(struct ext2_volume *vol, const char *path, k_u32_t mode, k_u32_t *number)
{
    struct ext2_inode parent;
    struct ext2_inode inode;
    char name[EXT2_NAME_MAX + 1];
    k_u32_t found;
    k_u32_t allocated = 0;
    int rc;

    if (vol == NULL || path == NULL || number == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    rc = lookup_parent(vol, path, &parent, name);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_dir_find(vol, &parent, name, &found);
    if (rc == 0) {
        return KERR_EXIST;
    }

    if (rc != KERR_NOENT) {
        return rc;
    }

    rc = ext2_alloc_inode(vol, &allocated);
    if (rc != 0) {
        return rc;
    }

    inode_new(&inode, allocated, (k_u16_t)(EXT2_S_IFREG | (mode & EXT2_MODE_PERM)), 1);

    rc = ext2_inode_write(vol, &inode, false);
    if (rc == 0) {
        rc = ext2_dir_insert(vol, &parent, name, allocated, 1);
    }

    if (rc != 0) {
        (void)ext2_free_inode(vol, allocated);
        return rc;
    }

    parent.mtime = ext2_now();
    parent.ctime = parent.mtime;

    rc = ext2_inode_write(vol, &parent, true);
    if (rc != 0) {
        return rc;
    }

    *number = allocated;

    return 0;
}

int ext2_make_dir(struct ext2_volume *vol, const char *path, k_u32_t mode)
{
    struct ext2_inode parent;
    struct ext2_inode inode;
    char name[EXT2_NAME_MAX + 1];
    k_u32_t found;
    k_u32_t allocated = 0;
    k_u32_t block = 0;
    int rc;

    if (vol == NULL || path == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    rc = lookup_parent(vol, path, &parent, name);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_dir_find(vol, &parent, name, &found);
    if (rc == 0) {
        return KERR_EXIST;
    }

    if (rc != KERR_NOENT) {
        return rc;
    }

    rc = ext2_alloc_inode(vol, &allocated);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_alloc_block(vol, &block);
    if (rc != 0) {
        (void)ext2_free_inode(vol, allocated);
        return rc;
    }

    /* The two entries every directory begins with, in one block. */
    memset(vol->block_buf, 0, vol->block_size);
    ext2_put32(vol->block_buf + 0, allocated);
    ext2_put16(vol->block_buf + 4, 12u);
    vol->block_buf[6] = 1u;
    vol->block_buf[7] = 2u;
    vol->block_buf[8] = '.';
    ext2_put32(vol->block_buf + 12, parent.number);
    ext2_put16(vol->block_buf + 16, (k_u16_t)(vol->block_size - 12u));
    vol->block_buf[18] = 2u;
    vol->block_buf[19] = 2u;
    vol->block_buf[20] = '.';
    vol->block_buf[21] = '.';

    /* The bit of the block reaches the medium before the block does. */
    rc = ext2_write_flush(vol);
    if (rc != 0) {
        (void)ext2_free_block(vol, block);
        (void)ext2_free_inode(vol, allocated);
        return rc;
    }

    rc = ext2_write_block(vol, block, vol->block_buf);
    if (rc != 0) {
        (void)ext2_free_block(vol, block);
        (void)ext2_free_inode(vol, allocated);
        return rc;
    }

    inode_new(&inode, allocated, (k_u16_t)(EXT2_S_IFDIR | (mode & EXT2_MODE_PERM)), 2);
    inode.size = vol->block_size;
    inode.blocks = block_sectors(vol);
    inode.block[0] = block;

    rc = ext2_inode_write(vol, &inode, false);
    if (rc != 0) {
        (void)ext2_free_block(vol, block);
        (void)ext2_free_inode(vol, allocated);
        return rc;
    }

    rc = group_add_dir(vol, allocated, 1);
    if (rc != 0) {
        (void)ext2_free_block(vol, block);
        (void)ext2_free_inode(vol, allocated);
        return rc;
    }

    rc = ext2_dir_insert(vol, &parent, name, allocated, 2);
    if (rc != 0) {
        (void)group_add_dir(vol, allocated, -1);
        (void)ext2_free_block(vol, block);
        (void)ext2_free_inode(vol, allocated);
        return rc;
    }

    /* ".." of the new directory is the parent, and the parent gains the
     * link of the entry it holds. */
    if (parent.links < 0xFFFFu) {
        parent.links++;
    }
    parent.mtime = ext2_now();
    parent.ctime = parent.mtime;

    rc = ext2_inode_write(vol, &parent, true);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

/*
 * One name of an inode has gone: the link count drops, the blocks go back to
 * the map when it was the last name, and the record is freed. The call is the
 * tail of both the removal and the replacement of a rename, and it is one
 * place because a second copy would be a second order of the writes.
 */
static int drop_inode(struct ext2_volume *vol, struct ext2_inode *inode)
{
    int rc;

    rc = ext2_truncate_inode(vol, inode, 0);
    if (rc != 0) {
        return rc;
    }

    if (inode->links > 0u) {
        inode->links--;
    }

    if (inode->links == 0u) {
        inode->dtime = ext2_now();
        inode->blocks = 0;
    }

    rc = ext2_inode_write(vol, inode, true);
    if (rc != 0) {
        return rc;
    }

    if (inode->links == 0u) {
        return ext2_free_inode(vol, inode->number);
    }

    return 0;
}

int ext2_remove(struct ext2_volume *vol, const char *path)
{
    struct ext2_inode parent;
    struct ext2_inode inode;
    char name[EXT2_NAME_MAX + 1];
    k_u32_t number;
    int rc;

    if (vol == NULL || path == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    rc = lookup_parent(vol, path, &parent, name);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_dir_find(vol, &parent, name, &number);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_read_inode(vol, number, &inode);
    if (rc != 0) {
        return rc;
    }

    if (ext2_inode_is_dir(&inode)) {
        return KERR_ISDIR;
    }

    if (!ext2_inode_is_file(&inode)) {
        return KERR_NOTSUP;
    }

    /* The name goes first: a block that no name reaches is an orphan that
     * e2fsck finds, while a name that reaches a freed inode is a lie of the
     * tree. */
    rc = ext2_dir_remove(vol, &parent, name);
    if (rc != 0) {
        return rc;
    }

    parent.mtime = ext2_now();
    parent.ctime = parent.mtime;

    rc = ext2_inode_write(vol, &parent, true);
    if (rc != 0) {
        return rc;
    }

    return drop_inode(vol, &inode);
}

/*
 * Remove an empty directory. The name goes first, then the links and the
 * blocks: a power cut between them leaves a record that no name reaches,
 * which the repair pass finds and frees, and never a name that points at the
 * blocks the map has already taken. The parent loses one link of its own -
 * the ".." of the removed directory is one - and the group loses one
 * directory in its count.
 */
int ext2_remove_dir(struct ext2_volume *vol, const char *path)
{
    struct ext2_inode parent;
    struct ext2_inode inode;
    char name[EXT2_NAME_MAX + 1];
    k_u32_t number;
    k_u64_t position = 0;
    struct ext2_dirent entry;
    int rc;

    if (vol == NULL || path == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    rc = lookup_parent(vol, path, &parent, name);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_dir_find(vol, &parent, name, &number);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_read_inode(vol, number, &inode);
    if (rc != 0) {
        return rc;
    }

    if (!ext2_inode_is_dir(&inode)) {
        return KERR_NOTDIR;
    }

    while ((rc = ext2_dir_next(vol, &inode, &position, &entry)) == 1) {
        if (strcmp(entry.name, ".") != 0 && strcmp(entry.name, "..") != 0) {
            return KERR_NOTEMPTY;
        }
    }

    if (rc < 0) {
        return rc;
    }

    rc = ext2_dir_remove(vol, &parent, name);
    if (rc != 0) {
        return rc;
    }

    if (parent.links > 0u) {
        parent.links--;
    }
    parent.mtime = ext2_now();
    parent.ctime = parent.mtime;

    rc = ext2_inode_write(vol, &parent, true);
    if (rc != 0) {
        return rc;
    }

    /* The truncate of a file refuses a directory on purpose; the blocks of a
     * directory are given back by the same walk of the direct and indirect
     * tables, and the size of the empty record becomes zero. */
    {
        k_u32_t freed = 0;

        rc = free_range(vol, &inode, 0, &freed);
        if (rc != 0) {
            return rc;
        }
    }

    inode.size = 0;
    inode.links = 0;
    inode.dtime = ext2_now();
    inode.blocks = 0;

    rc = ext2_inode_write(vol, &inode, true);
    if (rc != 0) {
        return rc;
    }

    rc = group_add_dir(vol, number, -1);
    if (rc != 0) {
        return rc;
    }

    return ext2_free_inode(vol, number);
}

/*
 * The same object under another name (ADR 0024 §5). A file moves to any
 * directory of the volume; a directory keeps its parent, because its ".."
 * entry and the tree below it name the place it came from. A name that is
 * already there and is a file is replaced; a directory at the destination is
 * refused with KERR_ISDIR, because the tree below it would be lost.
 *
 * The new name is written before the old one goes, the order MicroFS and FAT
 * keep: a volume that loses the power between the two has a name too many,
 * which a checker reports, and never an object that no name reaches.
 */
int ext2_rename(struct ext2_volume *vol, const char *from, const char *to)
{
    char from_dir_path[VFS_PATH_MAX];
    char from_name[EXT2_NAME_MAX + 1];
    char to_dir_path[VFS_PATH_MAX];
    char to_name[EXT2_NAME_MAX + 1];
    struct ext2_inode from_parent;
    struct ext2_inode to_parent;
    struct ext2_inode inode;
    struct ext2_inode replaced;
    k_u32_t number = 0;
    k_u32_t replaced_number = 0;
    bool same_parent;
    bool replacing = false;
    int rc;

    if (vol == NULL || from == NULL || to == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (strcmp(from, to) == 0) {
        return 0;
    }

    rc =
        ext2_split_parent(from, from_dir_path, sizeof(from_dir_path), from_name, sizeof(from_name));
    if (rc != 0) {
        return rc;
    }

    rc = ext2_split_parent(to, to_dir_path, sizeof(to_dir_path), to_name, sizeof(to_name));
    if (rc != 0) {
        return rc;
    }

    rc = lookup_parent(vol, from, &from_parent, from_name);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_dir_find(vol, &from_parent, from_name, &number);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_read_inode(vol, number, &inode);
    if (rc != 0) {
        return rc;
    }

    same_parent = strcmp(from_dir_path, to_dir_path) == 0;

    if (!same_parent && ext2_inode_is_dir(&inode)) {
        return KERR_NOTSUP;
    }

    rc = lookup_parent(vol, to, &to_parent, to_name);
    if (rc != 0) {
        return rc;
    }

    if (ext2_dir_find(vol, &to_parent, to_name, &replaced_number) == 0) {
        rc = ext2_read_inode(vol, replaced_number, &replaced);
        if (rc != 0) {
            return rc;
        }

        if (ext2_inode_is_dir(&replaced)) {
            return KERR_ISDIR;
        }

        replacing = true;
    }

    if (same_parent) {
        if (replacing) {
            rc = ext2_dir_remove(vol, &from_parent, to_name);
            if (rc != 0) {
                return rc;
            }
        }

        rc = ext2_dir_insert(vol, &from_parent, to_name, number, ext2_inode_is_dir(&inode) ? 2 : 1);
        if (rc != 0) {
            return rc;
        }

        rc = ext2_dir_remove(vol, &from_parent, from_name);
        if (rc != 0) {
            return rc;
        }

        from_parent.mtime = ext2_now();
        from_parent.ctime = from_parent.mtime;

        rc = ext2_inode_write(vol, &from_parent, true);
        if (rc != 0) {
            return rc;
        }
    } else {
        if (replacing) {
            rc = ext2_dir_remove(vol, &to_parent, to_name);
            if (rc != 0) {
                return rc;
            }
        }

        rc = ext2_dir_insert(vol, &to_parent, to_name, number, 1);
        if (rc != 0) {
            return rc;
        }

        to_parent.mtime = ext2_now();
        to_parent.ctime = to_parent.mtime;

        rc = ext2_inode_write(vol, &to_parent, true);
        if (rc != 0) {
            return rc;
        }

        rc = ext2_dir_remove(vol, &from_parent, from_name);
        if (rc != 0) {
            return rc;
        }

        from_parent.mtime = ext2_now();
        from_parent.ctime = from_parent.mtime;

        rc = ext2_inode_write(vol, &from_parent, true);
        if (rc != 0) {
            return rc;
        }
    }

    if (replacing) {
        rc = drop_inode(vol, &replaced);
        if (rc != 0) {
            return rc;
        }
    }

    /* The name moved and the content did not: ctime moves, mtime stays. */
    inode.ctime = ext2_now();

    return ext2_inode_write(vol, &inode, true);
}
