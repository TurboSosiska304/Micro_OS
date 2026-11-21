/*
 * Micro OS — the volume of an ext2 filesystem: the superblock, the table of
 * the group descriptors, the inodes and the stream of a file.
 *
 * The reader walks the format of ext2 and stops at its first sign of
 * trouble: a superblock that is not there, a block number outside the
 * volume, an inode outside the table. Nothing is repaired and nothing is
 * written; a damaged volume is described as it is (ADR 0019).
 */
#include "ext2_private.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/log.h"

/* The block of the group descriptors, the one after the superblock. */
static k_u64_t descriptor_start(const struct ext2_volume *vol)
{
    return (k_u64_t)(vol->first_data_block + 1u) * vol->block_size;
}

static bool is_power_of_two(k_u32_t value)
{
    return value != 0 && (value & (value - 1u)) == 0;
}

/* The bytes of the medium behind the volume, whatever the volume says. */
static int device_read(struct ext2_volume *vol, k_u64_t offset, void *buf, k_size_t size)
{
    k_u8_t *dst = (k_u8_t *)buf;
    k_u32_t dev_size = vol->dev->sector_size;
    k_u64_t device_bytes;

    if (dev_size == 0 || dev_size > EXT2_DEV_SECTOR_MAX || !is_power_of_two(dev_size)) {
        return KERR_NOTSUP;
    }

    if (vol->dev->ops == NULL || vol->dev->ops->read == NULL) {
        return KERR_NOTSUP;
    }

    device_bytes = (k_u64_t)vol->dev->sectors * dev_size;
    if (offset > device_bytes || size > device_bytes - offset) {
        return KERR_IO;
    }

    while (size > 0) {
        k_u32_t sector = (k_u32_t)(offset / dev_size);
        k_u32_t inside = (k_u32_t)(offset % dev_size);
        k_size_t chunk = dev_size - inside;

        if (sector >= vol->dev->sectors) {
            return KERR_IO;
        }

        if (chunk > size) {
            chunk = size;
        }

        if (!vol->cache_valid || vol->cached_sector != sector) {
            if (vol->dev->ops->read(vol->dev->ctx, sector, vol->sector, 1) != 0) {
                return KERR_IO;
            }
            vol->cached_sector = sector;
            vol->cache_valid = true;
        }

        memcpy(dst, vol->sector + inside, chunk);
        dst += chunk;
        offset += chunk;
        size -= chunk;
    }

    return 0;
}

/* Read bytes of the volume and refuse a request that runs past its end. */
int ext2_read_bytes(struct ext2_volume *vol, k_u64_t offset, void *buf, k_size_t size)
{
    if (vol == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (buf == NULL && size != 0) {
        return KERR_INVAL;
    }

    if (offset > vol->volume_bytes || size > vol->volume_bytes - offset) {
        return KERR_IO;
    }

    return device_read(vol, offset, buf, size);
}

int ext2_volume_mount(struct ext2_volume *vol, const struct k_blockdev *dev)
{
    k_u8_t sb[EXT2_SUPERBLOCK_SIZE];
    k_u8_t group[32];
    k_u32_t log_block_size;
    k_u32_t block_size;
    k_u32_t first_data_block;
    k_u32_t blocks_count;
    k_u32_t inodes_count;
    k_u32_t blocks_per_group;
    k_u32_t inodes_per_group;
    k_u32_t inode_size;
    k_u32_t rev_level;
    k_u32_t table;
    k_u64_t device_bytes;
    k_u64_t groups;
    int rc;

    if (vol == NULL || dev == NULL) {
        return KERR_INVAL;
    }

    k_mutex_destroy(&vol->lock);
    memset(vol, 0, sizeof(*vol));
    vol->dev = dev;

    /* The superblock is at the second kilobyte of every ext2 volume, so it
     * is read before the block size is known. */
    rc = device_read(vol, EXT2_SUPERBLOCK_OFFSET, sb, sizeof(sb));
    if (rc != 0) {
        return rc;
    }

    if (ext2_le16(sb + 56) != EXT2_SUPER_MAGIC) {
        return KERR_INVAL;
    }

    inodes_count = ext2_le32(sb + 0);
    blocks_count = ext2_le32(sb + 4);
    first_data_block = ext2_le32(sb + 20);
    log_block_size = ext2_le32(sb + 24);
    blocks_per_group = ext2_le32(sb + 32);
    inodes_per_group = ext2_le32(sb + 40);
    rev_level = ext2_le32(sb + 76);

    if (log_block_size > 2u) {
        return KERR_NOTSUP; /* the format stops at a block of 4096 bytes */
    }

    block_size = 1024u << log_block_size;

    if (blocks_count == 0 || inodes_count == 0 || blocks_per_group == 0 || inodes_per_group == 0 ||
        blocks_count <= first_data_block) {
        return KERR_INVAL;
    }

    if (first_data_block != ((block_size == 1024u) ? 1u : 0u)) {
        return KERR_INVAL;
    }

    if (rev_level > 1u) {
        return KERR_NOTSUP; /* the format has revision 0 and 1 */
    }

    if (rev_level == 0u) {
        inode_size = EXT2_INODE_MIN_SIZE;
    } else {
        inode_size = ext2_le16(sb + 88);
        if (inode_size < EXT2_INODE_MIN_SIZE || inode_size > block_size ||
            (block_size % inode_size) != 0) {
            return KERR_INVAL;
        }
    }

    groups = ((k_u64_t)blocks_count - first_data_block + blocks_per_group - 1u) / blocks_per_group;
    if (groups == 0 || groups > 0x10000u) {
        return KERR_NOTSUP;
    }

    vol->feature_compat = ext2_le32(sb + 92);
    vol->feature_incompat = ext2_le32(sb + 96);
    vol->feature_ro_compat = ext2_le32(sb + 100);

    /*
     * A journal is replayed by a writer, and this reader never writes: a
     * volume of ext3 that was not unmounted cleanly is not one this reader
     * can promise. An extent tree or a 64-bit block number is a different
     * layout, and the direct pointers below would read it as nonsense.
     */
    if ((vol->feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) != 0u) {
        return KERR_NOTSUP;
    }

    if ((vol->feature_incompat & ~EXT2_FEATURE_INCOMPAT_FILETYPE) != 0u) {
        return KERR_NOTSUP;
    }

    if ((vol->feature_ro_compat & EXT2_FEATURE_RO_COMPAT_HUGE_FILE) != 0u) {
        return KERR_NOTSUP;
    }

    vol->block_size = block_size;
    vol->first_data_block = first_data_block;
    vol->blocks_count = blocks_count;
    vol->inodes_count = inodes_count;
    vol->blocks_per_group = blocks_per_group;
    vol->inodes_per_group = inodes_per_group;
    vol->inode_size = inode_size;
    vol->groups = (k_u32_t)groups;
    vol->free_blocks = ext2_le32(sb + 12);
    vol->free_inodes = ext2_le32(sb + 16);
    vol->volume_bytes = (k_u64_t)blocks_count * block_size;

    device_bytes = (k_u64_t)dev->sectors * dev->sector_size;
    if (vol->volume_bytes < EXT2_SUPERBLOCK_OFFSET + EXT2_SUPERBLOCK_SIZE ||
        vol->volume_bytes > device_bytes) {
        return KERR_INVAL;
    }

    /* The table of the group descriptors has to fit on the medium before a
     * group is looked at; the descriptor of the first group names the inode
     * table, which is the next thing every path needs. */
    if (descriptor_start(vol) + (k_u64_t)groups * 32u > vol->volume_bytes) {
        return KERR_INVAL;
    }

    rc = device_read(vol, descriptor_start(vol), group, sizeof(group));
    if (rc != 0) {
        return rc;
    }

    table = ext2_le32(group + 8);
    if (table == 0 || table >= blocks_count) {
        return KERR_INVAL;
    }

    if ((k_u64_t)table * block_size + (k_u64_t)inodes_per_group * inode_size > vol->volume_bytes) {
        return KERR_INVAL;
    }

    vol->mounted = true;

    kinfo("ext2: %u inode(s), %u block(s) of %u bytes, %u group(s)", (unsigned)inodes_count,
          (unsigned)blocks_count, (unsigned)block_size, (unsigned)groups);

    return 0;
}

void ext2_volume_unmount(struct ext2_volume *vol)
{
    if (vol == NULL) {
        return;
    }

    vol->mounted = false;
    vol->cache_valid = false;
    k_mutex_destroy(&vol->lock);
}

const char *ext2_relative(const struct ext2_volume *vol, const char *path)
{
    k_size_t len;

    if (vol == NULL || path == NULL) {
        return NULL;
    }

    len = strlen(vol->point);
    if (strncmp(path, vol->point, len) != 0) {
        return NULL;
    }

    if (path[len] == '\0') {
        return "/";
    }

    if (path[len] != '/') {
        return NULL;
    }

    return path + len;
}

int ext2_read_inode(struct ext2_volume *vol, k_u32_t number, struct ext2_inode *out)
{
    k_u8_t raw[EXT2_INODE_MIN_SIZE];
    k_u8_t group[32];
    k_u32_t group_index;
    k_u32_t index;
    k_u32_t table;
    k_u64_t offset;
    k_u32_t i;
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (number == 0 || number > vol->inodes_count) {
        return KERR_IO;
    }

    group_index = (number - 1u) / vol->inodes_per_group;
    index = (number - 1u) % vol->inodes_per_group;

    if (group_index >= vol->groups) {
        return KERR_IO;
    }

    rc = ext2_read_bytes(vol, descriptor_start(vol) + (k_u64_t)group_index * 32u, group,
                         sizeof(group));
    if (rc != 0) {
        return rc;
    }

    table = ext2_le32(group + 8);
    if (table == 0 || table >= vol->blocks_count) {
        return KERR_IO;
    }

    offset = (k_u64_t)table * vol->block_size + (k_u64_t)index * vol->inode_size;
    if (offset + sizeof(raw) > vol->volume_bytes) {
        return KERR_IO;
    }

    rc = ext2_read_bytes(vol, offset, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    memset(out, 0, sizeof(*out));
    out->number = number;
    out->mode = ext2_le16(raw + 0);
    out->uid = ext2_le16(raw + 2);
    out->size = ext2_le32(raw + 4);
    out->atime = ext2_le32(raw + 8);
    out->ctime = ext2_le32(raw + 12);
    out->mtime = ext2_le32(raw + 16);
    out->gid = ext2_le16(raw + 24);
    out->links = ext2_le16(raw + 26);
    out->blocks = ext2_le32(raw + 28);
    out->flags = ext2_le32(raw + 32);

    for (i = 0; i < 15u; i++) {
        out->block[i] = ext2_le32(raw + 40u + i * 4u);
    }

    /* The high half of the size of a regular file lives in the field that
     * an older on-disk layout called the directory ACL. */
    if ((out->mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
        out->size |= (k_u64_t)ext2_le32(raw + 108) << 32;
    }

    if ((out->mode & EXT2_S_IFMT) == 0u) {
        return KERR_IO; /* an inode that no object of the volume used */
    }

    return 0;
}

/*
 * One entry of an indirect block. A table of zero is a hole; a block
 * number outside the volume is a table that the medium cannot hold.
 */
static int indirect_entry(struct ext2_volume *vol, k_u32_t table, k_u64_t index, k_u32_t *out)
{
    k_u8_t raw[4];
    k_u32_t value;
    k_u32_t pointers = vol->block_size / 4u;
    int rc;

    *out = 0;

    if (table == 0u) {
        return 0;
    }

    if (index >= pointers) {
        return KERR_IO;
    }

    if (table >= vol->blocks_count) {
        return KERR_IO;
    }

    rc = ext2_read_bytes(vol, (k_u64_t)table * vol->block_size + index * 4u, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    value = ext2_le32(raw);
    if (value == 0u) {
        return 0;
    }

    if (value >= vol->blocks_count) {
        return KERR_IO;
    }

    *out = value;
    return 0;
}

/*
 * The block that holds a logical block of a file: a direct entry, or one,
 * two or three tables of pointers. `block` is zero for a hole.
 */
int ext2_block_for_logical(struct ext2_volume *vol, const struct ext2_inode *inode, k_u64_t logical,
                           k_u32_t *block)
{
    k_u32_t pointers = vol->block_size / 4u;
    k_u64_t single = EXT2_DIRECT_BLOCKS + pointers;
    k_u64_t double_end = single + (k_u64_t)pointers * pointers;
    k_u64_t triple_end = double_end + (k_u64_t)pointers * pointers * pointers;
    k_u32_t first;
    k_u32_t second;
    k_u32_t value;
    int rc;

    *block = 0;

    if (logical < EXT2_DIRECT_BLOCKS) {
        value = inode->block[logical];
        if (value >= vol->blocks_count) {
            return KERR_IO;
        }
        *block = value;
        return 0;
    }

    if (logical < single) {
        rc = indirect_entry(vol, inode->block[12], logical - EXT2_DIRECT_BLOCKS, block);
        return rc;
    }

    if (logical < double_end) {
        k_u64_t index = logical - single;

        rc = indirect_entry(vol, inode->block[13], index / pointers, &first);
        if (rc != 0 || first == 0u) {
            return rc;
        }

        return indirect_entry(vol, first, index % pointers, block);
    }

    if (logical < triple_end) {
        k_u64_t index = logical - double_end;
        k_u64_t per_double = (k_u64_t)pointers * pointers;

        rc = indirect_entry(vol, inode->block[14], index / per_double, &first);
        if (rc != 0 || first == 0u) {
            return rc;
        }

        rc = indirect_entry(vol, first, (index / pointers) % pointers, &second);
        if (rc != 0 || second == 0u) {
            return rc;
        }

        return indirect_entry(vol, second, index % pointers, block);
    }

    return KERR_IO;
}

int ext2_read_file(struct ext2_volume *vol, const struct ext2_inode *inode, k_u64_t offset,
                   void *buf, k_size_t size, k_size_t *got)
{
    k_u8_t *dst = (k_u8_t *)buf;
    k_size_t want;
    k_size_t done = 0;
    int rc;

    if (vol == NULL || inode == NULL || got == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    *got = 0;

    if (!ext2_inode_is_file(inode)) {
        return KERR_ISDIR;
    }

    if (offset >= inode->size || size == 0) {
        return 0;
    }

    want = size;
    if ((k_u64_t)want > inode->size - offset) {
        want = (k_size_t)(inode->size - offset);
    }

    while (done < want) {
        k_u64_t position = offset + done;
        k_u64_t logical = position / vol->block_size;
        k_u32_t inside = (k_u32_t)(position % vol->block_size);
        k_size_t chunk = vol->block_size - inside;
        k_u32_t block = 0;

        if (chunk > want - done) {
            chunk = want - done;
        }

        rc = ext2_block_for_logical(vol, inode, logical, &block);
        if (rc != 0) {
            return rc;
        }

        if (block == 0u) {
            /* A sparse file has no bytes on the medium for this block. */
            memset(dst + done, 0, chunk);
        } else {
            rc = ext2_read_bytes(vol, (k_u64_t)block * vol->block_size + inside, dst + done, chunk);
            if (rc != 0) {
                return rc;
            }
        }

        done += chunk;
    }

    *got = done;
    return 0;
}

k_u8_t ext2_inode_vfs_type(const struct ext2_inode *inode)
{
    k_u16_t kind = inode->mode & EXT2_S_IFMT;

    if (kind == EXT2_S_IFDIR) {
        return 2;
    }

    if (kind == EXT2_S_IFREG) {
        return 1;
    }

    return 3; /* a device, a fifo, a socket or a symbolic link */
}
