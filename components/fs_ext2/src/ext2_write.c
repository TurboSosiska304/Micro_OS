/*
 * Micro OS — the write side of ext2: the bitmaps of a group, the inode of an
 * object, the records of a directory and the bytes of a file.
 *
 * The format is the one of ADR 0019 and it is not extended here: the
 * allocation walks the block and the inode bitmap of a group and keeps the
 * counters of the group and of the superblock true; an inode names its
 * blocks through the twelve direct pointers and one table of pointers,
 * which is what a root of this machine needs. A file that would have to
 * grow past that table answers KERR_NOSPC, the number of "no room", rather
 * than inventing a second layout.
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

/* Take a block from the bitmap of a group. */
static int group_alloc_block(struct ext2_volume *vol, struct ext2_group *group, k_u32_t *block)
{
    k_u32_t first = group_first_block(vol, group->number);
    k_u32_t bits = vol->blocks_per_group;
    k_u32_t i;
    int rc;

    if (first >= vol->blocks_count) {
        return KERR_NOSPC;
    }

    if (bits > vol->blocks_count - first) {
        bits = vol->blocks_count - first;
    }

    rc = ext2_read_block(vol, group->block_bitmap, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    for (i = 0; i < bits; i++) {
        if ((vol->block_buf[i / 8u] & (k_u8_t)(1u << (i % 8u))) == 0u) {
            break;
        }
    }

    if (i == bits) {
        return KERR_NOSPC; /* the counter of the group agrees with nothing */
    }

    vol->block_buf[i / 8u] |= (k_u8_t)(1u << (i % 8u));

    rc = ext2_write_block(vol, group->block_bitmap, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    if (group->free_blocks > 0u) {
        group->free_blocks--;
    }

    rc = ext2_group_write(vol, group);
    if (rc != 0) {
        return rc;
    }

    *block = first + i;

    if (vol->free_blocks > 0u) {
        vol->free_blocks--;
    }

    return 0;
}

int ext2_alloc_block(struct ext2_volume *vol, k_u32_t *block)
{
    k_u32_t start;
    k_u32_t tried;

    if (vol == NULL || block == NULL || !vol->mounted) {
        return KERR_INVAL;
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

        rc = group_alloc_block(vol, &group, block);
        if (rc == KERR_NOSPC) {
            continue;
        }
        if (rc != 0) {
            return rc;
        }

        /* The bytes of the object that used the block before are not the
         * bytes of the object that takes it now. */
        memset(vol->block_buf, 0, vol->block_size);
        rc = ext2_write_block(vol, *block, vol->block_buf);
        if (rc != 0) {
            return rc;
        }

        vol->alloc_hint = index;
        vol->dirty = true;

        return 0;
    }

    return KERR_NOSPC;
}

int ext2_free_block(struct ext2_volume *vol, k_u32_t block)
{
    struct ext2_group group;
    k_u32_t index;
    k_u32_t bit;
    k_u32_t first;
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
    bit = block - first;

    rc = ext2_read_block(vol, group.block_bitmap, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    if ((vol->block_buf[bit / 8u] & (k_u8_t)(1u << (bit % 8u))) == 0u) {
        return KERR_IO; /* the block was already free */
    }

    vol->block_buf[bit / 8u] &= (k_u8_t) ~(1u << (bit % 8u));

    rc = ext2_write_block(vol, group.block_bitmap, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

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
    k_u32_t i;
    int rc;

    if (first >= vol->inodes_count) {
        return KERR_NOSPC;
    }

    if (bits > vol->inodes_count - first) {
        bits = vol->inodes_count - first;
    }

    rc = ext2_read_block(vol, group->inode_bitmap, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    for (i = 0; i < bits; i++) {
        if ((vol->block_buf[i / 8u] & (k_u8_t)(1u << (i % 8u))) == 0u) {
            break;
        }
    }

    if (i == bits) {
        return KERR_NOSPC;
    }

    vol->block_buf[i / 8u] |= (k_u8_t)(1u << (i % 8u));

    rc = ext2_write_block(vol, group->inode_bitmap, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    if (group->free_inodes > 0u) {
        group->free_inodes--;
    }

    rc = ext2_group_write(vol, group);
    if (rc != 0) {
        return rc;
    }

    *number = first + i + 1u;

    if (vol->free_inodes > 0u) {
        vol->free_inodes--;
    }

    return 0;
}

int ext2_alloc_inode(struct ext2_volume *vol, k_u32_t *number)
{
    k_u32_t start;
    k_u32_t tried;

    if (vol == NULL || number == NULL || !vol->mounted) {
        return KERR_INVAL;
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

        vol->inode_hint = index;
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

    rc = ext2_read_block(vol, group.inode_bitmap, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    if ((vol->block_buf[index / 8u] & (k_u8_t)(1u << (index % 8u))) == 0u) {
        return KERR_IO;
    }

    vol->block_buf[index / 8u] &= (k_u8_t) ~(1u << (index % 8u));

    rc = ext2_write_block(vol, group.inode_bitmap, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    group.free_inodes++;
    rc = ext2_group_write(vol, &group);
    if (rc != 0) {
        return rc;
    }

    vol->free_inodes++;
    vol->dirty = true;

    return 0;
}

int ext2_inode_get_block(struct ext2_volume *vol, struct ext2_inode *inode, k_u64_t logical,
                         k_u32_t *block, bool create)
{
    k_u32_t pointers;

    if (vol == NULL || inode == NULL || block == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    pointers = vol->block_size / 4u;
    *block = 0;

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

    if (logical < EXT2_DIRECT_BLOCKS + pointers) {
        k_u32_t table = inode->block[12];
        k_u32_t entry;
        k_u8_t raw[4];
        k_u64_t offset;
        int rc;

        if (table == 0u) {
            if (!create) {
                return 0;
            }

            rc = ext2_alloc_block(vol, &table);
            if (rc != 0) {
                return rc;
            }

            inode->block[12] = table;
            inode->blocks += block_sectors(vol);
        }

        if (table >= vol->blocks_count) {
            return KERR_IO;
        }

        offset = (k_u64_t)table * vol->block_size + (k_u64_t)(logical - EXT2_DIRECT_BLOCKS) * 4u;

        rc = ext2_read_bytes(vol, offset, raw, sizeof(raw));
        if (rc != 0) {
            return rc;
        }

        entry = ext2_le32(raw);

        if (entry == 0u && create) {
            rc = ext2_alloc_block(vol, &entry);
            if (rc != 0) {
                return rc;
            }

            ext2_put32(raw, entry);
            rc = ext2_write_bytes(vol, offset, raw, sizeof(raw));
            if (rc != 0) {
                return rc;
            }

            inode->blocks += block_sectors(vol);
        }

        if (entry == 0u) {
            return 0;
        }

        if (entry >= vol->blocks_count) {
            return KERR_IO;
        }

        *block = entry;
        return 0;
    }

    /* A double or a triple table is ext2 that this writer does not make:
     * the plan leaves it to a later step, and a file that needs it gets
     * the answer of a volume that is full. */
    return KERR_NOSPC;
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
        k_size_t chunk = vol->block_size - inside;
        k_u32_t block = 0;

        if (chunk > size - done) {
            chunk = size - done;
        }

        rc = ext2_inode_get_block(vol, inode, logical, &block, true);
        if (rc != 0) {
            break;
        }

        if (block == 0u) {
            rc = KERR_IO;
            break;
        }

        if (inside == 0u && chunk == vol->block_size) {
            rc = ext2_write_block(vol, block, src + done);
        } else {
            rc = ext2_read_block(vol, block, vol->block_buf);
            if (rc == 0) {
                memcpy(vol->block_buf + inside, src + done, chunk);
                rc = ext2_write_block(vol, block, vol->block_buf);
            }
        }

        if (rc != 0) {
            break;
        }

        done += chunk;
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

        rc = ext2_inode_get_block(vol, inode, size / vol->block_size, &block, false);
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

    rc = ext2_truncate_inode(vol, &inode, 0);
    if (rc != 0) {
        return rc;
    }

    if (inode.links > 0u) {
        inode.links--;
    }

    if (inode.links == 0u) {
        inode.dtime = ext2_now();
        inode.blocks = 0;
    }

    rc = ext2_inode_write(vol, &inode, true);
    if (rc != 0) {
        return rc;
    }

    if (inode.links == 0u) {
        return ext2_free_inode(vol, number);
    }

    return 0;
}
