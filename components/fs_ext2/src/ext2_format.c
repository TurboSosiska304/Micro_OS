/*
 * Micro OS — the format of an ext2 volume.
 *
 * The installer formats the partition it just appended, on the target rather
 * than from a host-built image: the size and the table of the volume are the
 * ones of the medium. This writer lays down the smallest volume the reader of
 * this filesystem walks - a superblock, the descriptors, the two bitmaps, the
 * table of inodes, a root and a lost+found - and leaves the rest to the ordinary
 * writer once the volume is mounted (docs/plan-install.md, L3).
 *
 * The grain is the kilobyte of the release's images: the superblock is the
 * second kilobyte of the disk whatever the block, and a group of 8192 blocks
 * keeps the descriptor table in one block. The layout of every group is the
 * one e2fsck expects, so a volume made here is checked by the tool of the
 * world as well as by ext2_check() of this tree.
 */
#include "ext2_private.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/log.h"

#define FORMAT_BLOCK_SIZE 1024u
#define FORMAT_FIRST_DATA_BLOCK 1u
#define FORMAT_BLOCKS_PER_GROUP 8192u
#define FORMAT_INODE_SIZE EXT2_INODE_MIN_SIZE
#define FORMAT_INODES_PER_BLOCK (FORMAT_BLOCK_SIZE / FORMAT_INODE_SIZE)
#define FORMAT_INODES_MAX 1024u
#define FORMAT_RESERVED_INODES 11u
/* The first blocks of a group hold its backup superblock and descriptors;
 * a few more keep the bitmaps off the place e2fsck reserves for a backup. */
#define FORMAT_RESERVED_GDT 8u
#define FORMAT_ZERO_CHUNK 4096u

/* Bytes at `offset`, written through the sectors of the medium. A partial
 * sector is read and written back so a table next to it survives the call. */
static int format_write(const struct k_blockdev *dev, k_u64_t offset, const void *buf,
                        k_size_t size)
{
    const k_u8_t *src = (const k_u8_t *)buf;
    k_u32_t unit = dev->sector_size;
    k_u8_t sector[EXT2_DEV_SECTOR_MAX];

    while (size > 0u) {
        k_u32_t index = (k_u32_t)(offset / unit);
        k_u32_t inside = (k_u32_t)(offset % unit);
        k_size_t take = unit - inside;

        if (index >= dev->sectors) {
            return KERR_IO;
        }
        if (take > size) {
            take = size;
        }

        if (dev->ops->read(dev->ctx, index, sector, 1u) != 0) {
            return KERR_IO;
        }
        memcpy(sector + inside, src, take);
        if (dev->ops->write(dev->ctx, index, sector, 1u) != 0) {
            return KERR_IO;
        }

        src += take;
        offset += take;
        size -= take;
    }

    return 0;
}

/* Zero a run of blocks. The inode table of a large volume is mostly a hole,
 * and a stack buffer of one page keeps the writer independent of its size. */
static int format_zero(const struct k_blockdev *dev, k_u64_t offset, k_u64_t size)
{
    k_u8_t zeros[FORMAT_ZERO_CHUNK];

    memset(zeros, 0, sizeof(zeros));
    while (size > 0u) {
        k_size_t take = size > sizeof(zeros) ? sizeof(zeros) : (k_size_t)size;

        if (format_write(dev, offset, zeros, take) != 0) {
            return KERR_IO;
        }

        offset += take;
        size -= take;
    }

    return 0;
}

/* One bitmap: the bits of `used` objects at the front are set, the rest are
 * free. Both bitmaps of a fresh group have this shape; only the inode bitmap
 * of group zero keeps the reserved inodes. */
static int format_bitmap(const struct k_blockdev *dev, k_u32_t block, k_u32_t prefix, k_u32_t from,
                         k_u32_t to, k_u32_t total)
{
    k_u8_t bits[FORMAT_BLOCK_SIZE];
    k_u32_t i;

    memset(bits, 0, sizeof(bits));
    /* The bits past the last object of the group name nothing: the format
     * asks for them to be set, so a search for a free one never walks there. */
    for (i = total; i < sizeof(bits) * 8u; i++) {
        bits[i / 8u] |= (k_u8_t)(1u << (i % 8u));
    }
    for (i = 0u; i < prefix; i++) {
        bits[i / 8u] |= (k_u8_t)(1u << (i % 8u));
    }
    for (i = from; i < to; i++) {
        bits[i / 8u] |= (k_u8_t)(1u << (i % 8u));
    }

    return format_write(dev, (k_u64_t)block * FORMAT_BLOCK_SIZE, bits, sizeof(bits));
}

/* The two records of a directory root: "." and "..", with the second record
 * filling the rest of the block the way the format asks for. */
static int format_dot_entries(const struct k_blockdev *dev, k_u32_t block, k_u32_t self,
                              k_u32_t parent, bool with_lost_found)
{
    k_u8_t dir[FORMAT_BLOCK_SIZE];
    k_u32_t at = 0u;

    memset(dir, 0, sizeof(dir));

    /* The record is an inode, a length, a one-byte name length, a one-byte
     * file type and the name: the layout of a volume with the FILETYPE
     * feature, which is the one this writer declares in the superblock. */
    ext2_put32(dir + at, self);
    ext2_put16(dir + at + 4u, 12u);
    dir[at + 6u] = 1u;
    dir[at + 7u] = 2u; /* a directory */
    dir[at + 8u] = '.';
    at += 12u;

    ext2_put32(dir + at, parent);
    ext2_put16(dir + at + 4u, 12u);
    dir[at + 6u] = 2u;
    dir[at + 7u] = 2u;
    dir[at + 8u] = '.';
    dir[at + 9u] = '.';
    at += 12u;

    if (with_lost_found) {
        ext2_put32(dir + at, 11u);
        ext2_put16(dir + at + 4u, (k_u16_t)(FORMAT_BLOCK_SIZE - at));
        dir[at + 6u] = 10u;
        dir[at + 7u] = 2u;
        memcpy(dir + at + 8u, "lost+found", 10u);
    } else {
        ext2_put16(dir + at - 12u + 4u, (k_u16_t)(FORMAT_BLOCK_SIZE - (at - 12u)));
    }

    return format_write(dev, (k_u64_t)block * FORMAT_BLOCK_SIZE, dir, sizeof(dir));
}

/* An inode of the table, written as the 128 bytes of the format. */
static int format_inode(const struct k_blockdev *dev, k_u32_t table, k_u32_t number,
                        k_u32_t data_block, k_u16_t links)
{
    k_u8_t raw[FORMAT_INODE_SIZE];
    k_u32_t atime = ext2_now();

    memset(raw, 0, sizeof(raw));
    ext2_put16(raw + 0u, EXT2_S_IFDIR | EXT2_MODE_DIR);
    ext2_put16(raw + 2u, 0u); /* uid */
    ext2_put32(raw + 4u, FORMAT_BLOCK_SIZE);
    ext2_put32(raw + 8u, atime);
    ext2_put32(raw + 12u, atime);
    ext2_put32(raw + 16u, atime);
    ext2_put32(raw + 20u, 0u); /* dtime: alive */
    ext2_put16(raw + 24u, 0u); /* gid */
    ext2_put16(raw + 26u, links);
    ext2_put32(raw + 28u, FORMAT_BLOCK_SIZE / 512u);
    ext2_put32(raw + 32u, 0u); /* flags */
    ext2_put32(raw + 40u, data_block);

    return format_write(
        dev, (k_u64_t)table * FORMAT_BLOCK_SIZE + (k_u64_t)(number - 1u) * FORMAT_INODE_SIZE, raw,
        sizeof(raw));
}

/* The first block of the metadata of group `group`. Group zero carries the
 * superblock and the table of descriptors before its own bitmaps. */
static k_u32_t format_group_meta(k_u32_t group, k_u32_t desc_blocks)
{
    k_u32_t first = FORMAT_FIRST_DATA_BLOCK + group * FORMAT_BLOCKS_PER_GROUP;

    return first + 1u + desc_blocks + FORMAT_RESERVED_GDT;
}

static k_u32_t format_group_end(k_u32_t group, k_u32_t blocks)
{
    k_u64_t end =
        (k_u64_t)FORMAT_FIRST_DATA_BLOCK + (k_u64_t)(group + 1u) * FORMAT_BLOCKS_PER_GROUP;

    return end > blocks ? blocks : (k_u32_t)end;
}

int ext2_format(const struct k_blockdev *dev)
{
    k_u8_t super[EXT2_SUPERBLOCK_SIZE];
    k_u8_t group[32];
    k_u64_t media_bytes;
    k_u32_t blocks;
    k_u32_t groups;
    k_u32_t inodes_per_group;
    k_u32_t inodes;
    k_u32_t inode_blocks;
    k_u32_t desc_blocks;
    k_u32_t used = 0u;
    k_u32_t g;
    k_u32_t root_block;
    int rc;

    if (dev == NULL || dev->ops == NULL || dev->ops->read == NULL || dev->ops->write == NULL) {
        return KERR_INVAL;
    }

    /* The volume is a whole number of kilobytes and the sector divides the
     * block: a medium whose grain is wider cannot carry this layout. */
    if (dev->sector_size == 0u || dev->sector_size > FORMAT_BLOCK_SIZE ||
        (FORMAT_BLOCK_SIZE % dev->sector_size) != 0u) {
        return KERR_NOTSUP;
    }

    media_bytes = (k_u64_t)dev->sectors * dev->sector_size;
    if (media_bytes / FORMAT_BLOCK_SIZE > 0xFFFFFFFFu) {
        return KERR_NOTSUP;
    }
    blocks = (k_u32_t)(media_bytes / FORMAT_BLOCK_SIZE);
    if (blocks < 256u) {
        return KERR_INVAL; /* a volume too small to hold its own metadata */
    }

    groups =
        (blocks - FORMAT_FIRST_DATA_BLOCK + FORMAT_BLOCKS_PER_GROUP - 1u) / FORMAT_BLOCKS_PER_GROUP;
    if (groups == 0u || groups > 0x10000u) {
        return KERR_NOTSUP;
    }

    /* One inode per four kilobytes, capped at the table the images use. The
     * count is a multiple of the inodes in one block, so every group's table
     * is a whole number of blocks. */
    inodes_per_group = (blocks - FORMAT_FIRST_DATA_BLOCK) / (groups * 4u);
    if (inodes_per_group > FORMAT_INODES_MAX) {
        inodes_per_group = FORMAT_INODES_MAX;
    }
    inodes_per_group -= inodes_per_group % FORMAT_INODES_PER_BLOCK;
    if (inodes_per_group < FORMAT_INODES_PER_BLOCK) {
        inodes_per_group = FORMAT_INODES_PER_BLOCK;
    }
    inodes = groups * inodes_per_group;
    inode_blocks = inodes_per_group * FORMAT_INODE_SIZE / FORMAT_BLOCK_SIZE;
    desc_blocks = (groups * 32u + FORMAT_BLOCK_SIZE - 1u) / FORMAT_BLOCK_SIZE;

    /* Validate every group before a byte is written: a volume whose last
     * group cannot hold its table is refused whole, not half formatted. The
     * superblock and the descriptors of a group are used; the reserved GDT
     * blocks are not, so a bitmap only marks the two runs below. */
    for (g = 0u; g < groups; g++) {
        k_u32_t meta = format_group_meta(g, desc_blocks);
        k_u32_t end = format_group_end(g, blocks);
        k_u32_t from = 1u + desc_blocks + FORMAT_RESERVED_GDT;
        k_u32_t to = from + 2u + inode_blocks + (g == 0u ? 2u : 0u);

        if (meta + 2u + inode_blocks + (g == 0u ? 2u : 0u) >= end) {
            return KERR_NOSPC;
        }
        used += 1u + desc_blocks + (to - from);
    }

    root_block = format_group_meta(0u, desc_blocks) + 2u + inode_blocks;

    /* The empty volume: the boot block, the superblock block, the descriptors
     * and the metadata of every group are zeros until written below. */
    rc = format_zero(dev, 0u, (k_u64_t)root_block * FORMAT_BLOCK_SIZE);
    for (g = 1u; g < groups && rc == 0u; g++) {
        k_u32_t meta = format_group_meta(g, desc_blocks);
        k_u32_t first = FORMAT_FIRST_DATA_BLOCK + g * FORMAT_BLOCKS_PER_GROUP;

        rc = format_zero(dev, (k_u64_t)first * FORMAT_BLOCK_SIZE,
                         (k_u64_t)(meta + 2u + inode_blocks - first) * FORMAT_BLOCK_SIZE);
    }
    if (rc != 0u) {
        return rc;
    }

    for (g = 0u; g < groups; g++) {
        k_u32_t meta = format_group_meta(g, desc_blocks);
        k_u32_t first = FORMAT_FIRST_DATA_BLOCK + g * FORMAT_BLOCKS_PER_GROUP;
        k_u32_t end = format_group_end(g, blocks);
        k_u32_t prefix = 1u + desc_blocks;
        k_u32_t from = prefix + FORMAT_RESERVED_GDT;
        k_u32_t to = from + 2u + inode_blocks + (g == 0u ? 2u : 0u);
        k_u32_t group_used = prefix + (to - from);

        rc = format_bitmap(dev, meta, prefix, from, to, end - first);
        if (rc != 0u) {
            return rc;
        }
        rc = format_bitmap(dev, meta + 1u, g == 0u ? FORMAT_RESERVED_INODES : 0u, 0u, 0u,
                           inodes_per_group);
        if (rc != 0u) {
            return rc;
        }

        memset(group, 0, sizeof(group));
        ext2_put32(group + 0u, meta);
        ext2_put32(group + 4u, meta + 1u);
        ext2_put32(group + 8u, meta + 2u);
        ext2_put16(group + 12u, (k_u16_t)(end - first - group_used));
        ext2_put16(group + 14u,
                   (k_u16_t)(inodes_per_group - (g == 0u ? FORMAT_RESERVED_INODES : 0u)));
        ext2_put16(group + 16u, (k_u16_t)(g == 0u ? 2u : 0u));
        rc = format_write(
            dev, (k_u64_t)(FORMAT_FIRST_DATA_BLOCK + 1u) * FORMAT_BLOCK_SIZE + (k_u64_t)g * 32u,
            group, sizeof(group));
        if (rc != 0u) {
            return rc;
        }
    }

    rc =
        format_inode(dev, format_group_meta(0u, desc_blocks) + 2u, EXT2_ROOT_INODE, root_block, 3u);
    if (rc != 0u) {
        return rc;
    }
    rc = format_inode(dev, format_group_meta(0u, desc_blocks) + 2u, 11u, root_block + 1u, 2u);
    if (rc != 0u) {
        return rc;
    }
    rc = format_dot_entries(dev, root_block, EXT2_ROOT_INODE, EXT2_ROOT_INODE, true);
    if (rc != 0u) {
        return rc;
    }
    rc = format_dot_entries(dev, root_block + 1u, 11u, EXT2_ROOT_INODE, false);
    if (rc != 0u) {
        return rc;
    }

    memset(super, 0, sizeof(super));
    ext2_put32(super + 0u, inodes);
    ext2_put32(super + 4u, blocks);
    ext2_put32(super + 8u, 0u); /* reserved blocks */
    ext2_put32(super + 12u, blocks - FORMAT_FIRST_DATA_BLOCK - used);
    ext2_put32(super + 16u, inodes - FORMAT_RESERVED_INODES);
    ext2_put32(super + 20u, FORMAT_FIRST_DATA_BLOCK);
    ext2_put32(super + 24u, 0u); /* 1024-byte blocks */
    ext2_put32(super + 28u, 0u); /* 1024-byte fragments */
    ext2_put32(super + 32u, FORMAT_BLOCKS_PER_GROUP);
    ext2_put32(super + 36u, FORMAT_BLOCKS_PER_GROUP);
    ext2_put32(super + 40u, inodes_per_group);
    ext2_put32(super + 44u, ext2_now());
    ext2_put32(super + 48u, ext2_now());
    ext2_put16(super + 52u, 0u);  /* mount count */
    ext2_put16(super + 54u, 20u); /* check interval */
    ext2_put16(super + 56u, EXT2_SUPER_MAGIC);
    ext2_put16(super + 58u, 1u); /* clean */
    ext2_put16(super + 60u, 1u); /* continue on error */
    ext2_put16(super + 62u, 0u); /* minor revision */
    ext2_put32(super + 64u, ext2_now());
    ext2_put32(super + 68u, 0u);
    ext2_put32(super + 72u, 0u); /* Linux */
    ext2_put32(super + 76u, 0u); /* revision 0: 128-byte inodes */
    ext2_put16(super + 80u, 0u);
    ext2_put16(super + 82u, 0u);
    ext2_put32(super + 84u, FORMAT_RESERVED_INODES);
    ext2_put16(super + 88u, FORMAT_INODE_SIZE);
    ext2_put32(super + 92u, 0u);                             /* compat */
    ext2_put32(super + 96u, EXT2_FEATURE_INCOMPAT_FILETYPE); /* incompat */
    ext2_put32(super + 100u, 0u);                            /* read-only compat */
    memset(super + 104u, 0, 16u);                            /* UUID */
    memcpy(super + 120u, "MICROOS", 7u);                     /* volume name */

    rc = format_write(dev, EXT2_SUPERBLOCK_OFFSET, super, sizeof(super));
    if (rc != 0u) {
        return rc;
    }

    if (dev->ops->sync != NULL && dev->ops->sync(dev->ctx) != 0u) {
        return KERR_IO;
    }

    kinfo("ext2: formatted %u block(s) of %u bytes, %u inode(s), %u group(s)", (unsigned)blocks,
          (unsigned)FORMAT_BLOCK_SIZE, (unsigned)inodes, (unsigned)groups);

    return 0;
}
