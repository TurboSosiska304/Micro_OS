/*
 * Micro OS — the volume of an ext2 filesystem: the superblock, the table of
 * the group descriptors, the inodes and the stream of a file.
 *
 * The reader walks the format of ext2 and stops at its first sign of
 * trouble: a superblock that is not there, a block number outside the
 * volume, an inode outside the table. Nothing is repaired; a damaged volume
 * is described as it is (ADR 0019). The primitives of a write - the bytes of
 * the medium, a block, the counters of the superblock - live here too, and
 * the shape of the changes is the business of ext2_write.c (ADR 0029).
 */
#include "ext2_private.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/log.h"

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
        k_u32_t sector;
        k_u32_t inside;
        k_size_t chunk;

        /*
         * A run of whole sectors goes to the device in one call. The block
         * layer above a device gathers the run into the commands of a read
         * ahead or of a bus master, and the loop below would ask for the
         * same sectors one at a time; a file is read in runs of tens of
         * kilobytes and the difference is the commands of the medium
         * (ADR 0037).
         */
        if ((offset % dev_size) == 0u && size >= dev_size) {
            k_size_t run = size / dev_size;

            if (run > EXT2_IO_BYTES_MAX / dev_size) {
                run = EXT2_IO_BYTES_MAX / dev_size;
            }

            if (vol->dev->ops->read(vol->dev->ctx, (k_u32_t)(offset / dev_size), dst, run) != 0) {
                return KERR_IO;
            }

            dst += run * dev_size;
            offset += run * dev_size;
            size -= run * dev_size;
            continue;
        }

        sector = (k_u32_t)(offset / dev_size);
        inside = (k_u32_t)(offset % dev_size);
        chunk = dev_size - inside;

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

/* The bytes of the medium, written as they are. */
static int device_write(struct ext2_volume *vol, k_u64_t offset, const void *buf, k_size_t size)
{
    const k_u8_t *src = (const k_u8_t *)buf;
    k_u32_t dev_size = vol->dev->sector_size;
    k_u64_t device_bytes;

    if (dev_size == 0 || dev_size > EXT2_DEV_SECTOR_MAX || !is_power_of_two(dev_size)) {
        return KERR_NOTSUP;
    }

    if (vol->dev->ops == NULL || vol->dev->ops->write == NULL) {
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

        if (inside == 0 && chunk == dev_size) {
            if (vol->dev->ops->write(vol->dev->ctx, sector, src, 1) != 0) {
                return KERR_IO;
            }
        } else {
            /* A partial sector is read, changed and written back. */
            if (vol->dev->ops->read(vol->dev->ctx, sector, vol->sector, 1) != 0) {
                return KERR_IO;
            }
            memcpy(vol->sector + inside, src, chunk);
            if (vol->dev->ops->write(vol->dev->ctx, sector, vol->sector, 1) != 0) {
                return KERR_IO;
            }
        }

        /* The cache holds what the medium held before the write. */
        vol->cache_valid = false;

        src += chunk;
        offset += chunk;
        size -= chunk;
    }

    return 0;
}

/* Write bytes of the volume and refuse a request that runs past its end. */
int ext2_write_bytes(struct ext2_volume *vol, k_u64_t offset, const void *buf, k_size_t size)
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

    return device_write(vol, offset, buf, size);
}

/* One block of the volume, by its number. */
int ext2_read_block(struct ext2_volume *vol, k_u32_t block, void *buf)
{
    if (vol == NULL || !vol->mounted || (buf == NULL)) {
        return KERR_INVAL;
    }

    if (block >= vol->blocks_count) {
        return KERR_IO;
    }

    return ext2_read_bytes(vol, (k_u64_t)block * vol->block_size, buf, vol->block_size);
}

int ext2_write_block(struct ext2_volume *vol, k_u32_t block, const void *buf)
{
    if (vol == NULL || !vol->mounted || (buf == NULL)) {
        return KERR_INVAL;
    }

    if (block >= vol->blocks_count) {
        return KERR_IO;
    }

    return ext2_write_bytes(vol, (k_u64_t)block * vol->block_size, buf, vol->block_size);
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
     * A journal is replayed by a writer of ext3, which this filesystem is
     * not: a volume of ext3 that was not unmounted cleanly is not one this
     * mount can promise. An extent tree or a 64-bit block number is a
     * different layout, and the direct pointers below would read it as
     * nonsense.
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
    /* No group of the new volume is in the slot of the writer yet. */
    vol->group_cached = vol->groups;
    vol->bitmap_kind = EXT2_BITMAP_NONE;
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
    if (ext2_descriptor_start(vol) + (k_u64_t)groups * 32u > vol->volume_bytes) {
        return KERR_INVAL;
    }

    rc = device_read(vol, ext2_descriptor_start(vol), group, sizeof(group));
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

/*
 * The counters of the superblock are the ones of the volume that was
 * mounted, and the writer changes the medium only when it has a reason:
 * a volume that was read and left alone is left exactly as it was.
 *
 * The order is the one e2fsck reads: the bitmap and the descriptor of the
 * group of the last change, then the counters of the volume, then the cache
 * of the drive. A session that settles here leaves a volume the tool of the
 * format accepts.
 */
int ext2_volume_flush(struct ext2_volume *vol)
{
    k_u8_t sb[EXT2_SUPERBLOCK_SIZE];
    int rc;

    if (vol == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (!vol->dirty && !vol->bitmap_dirty && !vol->group_dirty) {
        return 0;
    }

    rc = ext2_write_flush(vol);
    if (rc != 0) {
        return rc;
    }

    rc = ext2_read_bytes(vol, EXT2_SUPERBLOCK_OFFSET, sb, sizeof(sb));
    if (rc != 0) {
        return rc;
    }

    ext2_put32(sb + 12, vol->free_blocks);
    ext2_put32(sb + 16, vol->free_inodes);
    ext2_put32(sb + 48, ext2_now()); /* the last moment of a write */

    rc = ext2_write_bytes(vol, EXT2_SUPERBLOCK_OFFSET, sb, sizeof(sb));
    if (rc != 0) {
        return rc;
    }

    if (vol->dev->ops != NULL && vol->dev->ops->sync != NULL) {
        if (vol->dev->ops->sync(vol->dev->ctx) != 0) {
            return KERR_IO;
        }
    }

    vol->dirty = false;

    return 0;
}

void ext2_volume_unmount(struct ext2_volume *vol)
{
    if (vol == NULL) {
        return;
    }

    if (vol->mounted) {
        (void)ext2_volume_flush(vol);
        vol->group_cached = vol->groups;
        vol->bitmap_kind = EXT2_BITMAP_NONE;
    }

    vol->mounted = false;
    vol->cache_valid = false;
    k_mutex_destroy(&vol->lock);
}

/* The descriptor of one group, as the 32 bytes it is on the medium. */
static int group_read_medium(struct ext2_volume *vol, k_u32_t index, struct ext2_group *out)
{
    k_u8_t raw[32];
    int rc;

    rc = ext2_read_bytes(vol, ext2_descriptor_start(vol) + (k_u64_t)index * 32u, raw, sizeof(raw));
    if (rc != 0) {
        return rc;
    }

    memset(out, 0, sizeof(*out));
    out->number = index;
    out->block_bitmap = ext2_le32(raw + 0);
    out->inode_bitmap = ext2_le32(raw + 4);
    out->inode_table = ext2_le32(raw + 8);
    out->free_blocks = ext2_le16(raw + 12);
    out->free_inodes = ext2_le16(raw + 14);
    out->used_dirs = ext2_le16(raw + 16);

    return 0;
}

/* The descriptor of one group. The slot of the volume answers for the group
 * of the last change without a read; a group that is not the one of the slot
 * settles the slot first, because the counters of a group that leaves the
 * memory are the counters of the medium. */
int ext2_group_read(struct ext2_volume *vol, k_u32_t index, struct ext2_group *out)
{
    int rc;

    if (vol == NULL || out == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (index >= vol->groups) {
        return KERR_INVAL;
    }

    if (vol->group_cached != index) {
        rc = ext2_write_flush(vol);
        if (rc != 0) {
            return rc;
        }

        rc = group_read_medium(vol, index, &vol->group);
        if (rc != 0) {
            vol->group_cached = vol->groups;
            vol->bitmap_kind = EXT2_BITMAP_NONE;
            return rc;
        }

        vol->group_cached = index;
        vol->group_dirty = false;
        vol->bitmap_kind = EXT2_BITMAP_NONE;
        vol->bitmap_dirty = false;
    }

    *out = vol->group;

    return 0;
}

int ext2_group_write(struct ext2_volume *vol, const struct ext2_group *group)
{
    struct ext2_group ignored;

    if (vol == NULL || group == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (group->number >= vol->groups || group->block_bitmap >= vol->blocks_count ||
        group->inode_bitmap >= vol->blocks_count || group->inode_table >= vol->blocks_count) {
        return KERR_IO;
    }

    if (vol->group_cached != group->number) {
        /* A descriptor that is not the one of the slot: the slot is loaded
         * with it, which settles the group it held. */
        int rc = ext2_group_read(vol, group->number, &ignored);

        if (rc != 0) {
            return rc;
        }
    }

    vol->group = *group;
    vol->group_dirty = true;
    vol->dirty = true;

    return 0;
}

/* The 32 bytes of a descriptor, written as the medium keeps them. */
static int group_write_medium(struct ext2_volume *vol, const struct ext2_group *group)
{
    k_u8_t raw[32];

    memset(raw, 0, sizeof(raw));
    ext2_put32(raw + 0, group->block_bitmap);
    ext2_put32(raw + 4, group->inode_bitmap);
    ext2_put32(raw + 8, group->inode_table);
    ext2_put16(raw + 12, group->free_blocks);
    ext2_put16(raw + 14, group->free_inodes);
    ext2_put16(raw + 16, group->used_dirs);

    return ext2_write_bytes(vol, ext2_descriptor_start(vol) + (k_u64_t)group->number * 32u, raw,
                            sizeof(raw));
}

/*
 * The two changes of a group that the writer holds back: the bitmap it
 * changed and the counters of the descriptor. Both are written here, the
 * bitmap first - the bits of a block are the record that the descriptor
 * sums - and the caller of the flush is a settle, a change of group or the
 * end of a run of the medium.
 */
int ext2_write_flush(struct ext2_volume *vol)
{
    int rc;

    if (vol == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    if (vol->bitmap_dirty && vol->bitmap_kind != EXT2_BITMAP_NONE) {
        k_u32_t block = (vol->bitmap_kind == EXT2_BITMAP_INODE) ? vol->group.inode_bitmap
                                                                : vol->group.block_bitmap;

        rc = ext2_write_block(vol, block, vol->bitmap);
        if (rc != 0) {
            return rc;
        }

        vol->bitmap_dirty = false;
    }

    if (vol->group_dirty) {
        rc = group_write_medium(vol, &vol->group);
        if (rc != 0) {
            return rc;
        }

        vol->group_dirty = false;
    }

    return 0;
}

int ext2_write_state_drop(struct ext2_volume *vol)
{
    int rc;

    if (vol == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    rc = ext2_write_flush(vol);
    if (rc != 0) {
        return rc;
    }

    vol->group_cached = vol->groups;
    vol->bitmap_kind = EXT2_BITMAP_NONE;
    vol->bitmap_dirty = false;
    vol->group_dirty = false;

    return 0;
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

    /*
     * The root of the tree is the one point of one character: the VFS hands
     * a path below it with its own slash ("/bin"), and that path is already
     * the relative one (ADR 0027, a root of ext2). A point with a name keeps
     * its separator and the walk below starts after it.
     */
    if (len == 1u) {
        return path[0] == '/' ? path : NULL;
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

    rc = ext2_read_bytes(vol, ext2_descriptor_start(vol) + (k_u64_t)group_index * 32u, group,
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
    out->dtime = ext2_le32(raw + 20);
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

/*
 * How many logical blocks the table of pointers of `logical` holds from it
 * on: the twelve entries of the inode, or the entries of one table of the
 * single, the double and the triple indirection. A run never crosses one of
 * them, because the entries of a run are read and written as one block.
 */
static k_u32_t run_limit(const struct ext2_volume *vol, k_u64_t logical)
{
    k_u64_t pointers = vol->block_size / 4u;
    k_u64_t double_start = EXT2_DIRECT_BLOCKS + pointers;
    k_u64_t triple_start = double_start + pointers * pointers;

    if (logical < EXT2_DIRECT_BLOCKS) {
        return (k_u32_t)(EXT2_DIRECT_BLOCKS - logical);
    }

    if (logical < double_start) {
        return (k_u32_t)(pointers - (logical - EXT2_DIRECT_BLOCKS) % pointers);
    }

    if (logical < triple_start) {
        return (k_u32_t)(pointers - (logical - double_start) % pointers);
    }

    return (k_u32_t)(pointers - (logical - triple_start) % pointers);
}

/*
 * The table that names the blocks of the logical block `index` under
 * `table`, and the entry of `index` in it. `level` is the height of the tree
 * below `table`: a table of the single indirection is one, the tables of the
 * double and of the triple are two and three. `leaf` is zero when a table on
 * the way is a hole, which is every block of the subtree being a hole.
 *
 * The walk reads one entry per level and not one per block: the entries of
 * all the blocks of a run are in the same leaf table, and `ext2_block_run`
 * reads that table once.
 */
static int leaf_table(struct ext2_volume *vol, k_u32_t table, unsigned level, k_u64_t index,
                      k_u32_t *leaf, k_u64_t *entry)
{
    k_u32_t pointers = vol->block_size / 4u;
    k_u64_t stride = 1;
    unsigned i;

    *leaf = 0;
    *entry = 0;

    for (i = 1; i < level; i++) {
        stride *= pointers;
    }

    if (level == 0u || level > 3u || index >= stride * pointers) {
        return KERR_IO;
    }

    if (table == 0u) {
        return 0;
    }

    if (table >= vol->blocks_count) {
        return KERR_IO;
    }

    if (level == 1u) {
        *leaf = table;
        *entry = index;
        return 0;
    }

    {
        k_u8_t raw[4];
        k_u32_t below;

        if (ext2_read_bytes(vol, (k_u64_t)table * vol->block_size + (index / stride) * 4u, raw,
                            sizeof(raw)) != 0) {
            return KERR_IO;
        }

        below = ext2_le32(raw);

        if (below >= vol->blocks_count) {
            return KERR_IO;
        }

        return leaf_table(vol, below, level - 1u, index % stride, leaf, entry);
    }
}

int ext2_block_run(struct ext2_volume *vol, const struct ext2_inode *inode, k_u64_t logical,
                   k_u32_t want, k_u32_t *first, k_u32_t *run)
{
    k_u64_t pointers;
    k_u64_t double_start;
    k_u64_t triple_start;
    k_u32_t table = 0;
    k_u64_t entry = 0;
    k_u32_t base = 0;
    k_u32_t most;
    k_u32_t n = 1;
    k_u32_t i;
    int rc;

    if (vol == NULL || inode == NULL || first == NULL || run == NULL || !vol->mounted) {
        return KERR_INVAL;
    }

    *first = 0;
    *run = 0;

    pointers = vol->block_size / 4u;
    double_start = EXT2_DIRECT_BLOCKS + pointers;
    triple_start = double_start + pointers * pointers;

    most = run_limit(vol, logical);
    if (most > EXT2_IO_BYTES_MAX / vol->block_size) {
        most = (k_u32_t)(EXT2_IO_BYTES_MAX / vol->block_size);
    }
    if (want > most) {
        want = most;
    }

    if (logical < EXT2_DIRECT_BLOCKS) {
        base = inode->block[logical];
        entry = 0;
        table = 0; /* the entries of a run of the inode are in the inode */
    } else if (logical < double_start) {
        rc = leaf_table(vol, inode->block[12], 1u, logical - EXT2_DIRECT_BLOCKS, &table, &entry);
        if (rc != 0) {
            return rc;
        }
    } else if (logical < triple_start) {
        rc = leaf_table(vol, inode->block[13], 2u, logical - double_start, &table, &entry);
        if (rc != 0) {
            return rc;
        }
    } else {
        rc = leaf_table(vol, inode->block[14], 3u, logical - triple_start, &table, &entry);
        if (rc != 0) {
            return rc;
        }
    }

    if (table == 0u) {
        if (logical < EXT2_DIRECT_BLOCKS) {
            /* The entries of the inode are in the inode itself. */
            if (base >= vol->blocks_count) {
                return KERR_IO;
            }

            if (base == 0u) {
                for (; n < want; n++) {
                    if (inode->block[logical + n] != 0u) {
                        break;
                    }
                }
            } else {
                for (; n < want; n++) {
                    if (inode->block[logical + n] != base + n) {
                        break;
                    }
                }
            }

            *first = base;
            *run = n;
            return 0;
        }

        /* A table that is not there is a hole of the whole subtree. */
        *first = 0;
        *run = want;
        return 0;
    }

    /* One read of the leaf table, and the walk of the entries in the memory
     * of the volume: a run of a file is fifteen to a few hundred entries of
     * one block, and a read of four bytes for every block of a stream is the
     * cost this call removes. */
    rc = ext2_read_block(vol, table, vol->block_buf);
    if (rc != 0) {
        return rc;
    }

    base = ext2_le32(vol->block_buf + (k_u32_t)entry * 4u);
    if (base >= vol->blocks_count) {
        return KERR_IO;
    }

    for (i = 1; i < want; i++) {
        k_u32_t block = ext2_le32(vol->block_buf + (k_u32_t)(entry + i) * 4u);

        if (block >= vol->blocks_count) {
            return KERR_IO;
        }

        /* A run is a hold of holes or a chain of blocks that come one after
         * another; an entry that is neither ends it. */
        if (base == 0u) {
            if (block != 0u) {
                break;
            }
        } else if (block != base + i) {
            break;
        }

        n++;
    }

    *first = base;
    *run = n;

    return 0;
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
        k_u32_t block = 0;
        k_u32_t run = 0;
        k_u32_t most;
        k_size_t bytes;

        /* The blocks that the file holds one after another are one call of
         * the medium and one copy of the block layer, not a block at a time:
         * the run is the whole of what is left of the call when the blocks
         * of the file are the blocks of the volume. */
        most = (k_u32_t)((want - done + vol->block_size - 1u) / vol->block_size);
        rc = ext2_block_run(vol, inode, logical, most, &block, &run);
        if (rc != 0) {
            return rc;
        }

        if (run == 0u) {
            return KERR_IO;
        }

        bytes = (k_size_t)run * vol->block_size - inside;
        if (bytes > want - done) {
            bytes = want - done;
        }

        if (block == 0u) {
            /* A sparse file has no bytes on the medium for this block. */
            memset(dst + done, 0, bytes);
        } else {
            rc = ext2_read_bytes(vol, (k_u64_t)block * vol->block_size + inside, dst + done, bytes);
            if (rc != 0) {
                return rc;
            }
        }

        done += bytes;
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
