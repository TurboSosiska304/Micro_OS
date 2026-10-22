#include "mfs/mfs.h"

#include <string.h>

#include "kernel/log.h"

/*
 * The state of the mounted volume. The filesystem is a singleton: one
 * volume at a time, like the flash of the board.
 */
static struct {
    bool mounted;
    struct mfs_device dev;
    struct mfs_superblock sb;
    k_u32_t bitmap_blocks;  /* the blocks of the bitmap of the free blocks */
    k_u32_t inode_blocks;   /* the blocks of the table of the inodes */
    k_u32_t inode_capacity; /* the number of the records of the table */
    bool bitmap_dirty;      /* the map in the memory is newer than the device */
    k_u8_t bitmap[MFS_BLOCK_SIZE * 2];
} g;

/*
 * Every data block starts with the number of the next block of the chain.
 * The end of the chain is marked with MFS_NEXT_NONE, so that a file can grow
 * without the blocks being continuous.
 */
#define MFS_NEXT_NONE 0xFFFFFFFFu
#define MFS_BLOCK_DATA (MFS_BLOCK_SIZE - 4)

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

/* --- the low level access to the volume --- */

static int dev_read(k_u32_t block, void *buf)
{
    if (block >= g.sb.total_blocks) {
        return MFS_EIO;
    }

    if (g.dev.read_block(g.dev.ctx, block, buf) != 0) {
        return MFS_EIO;
    }

    return MFS_OK;
}

static int dev_write(k_u32_t block, const void *buf)
{
    if (block >= g.sb.total_blocks) {
        return MFS_EIO;
    }

    if (g.dev.write_block(g.dev.ctx, block, buf) != 0) {
        return MFS_EIO;
    }

    return MFS_OK;
}

/* --- the bitmap of the free blocks --- */

static k_u32_t bitmap_bytes(void)
{
    return (g.sb.data_blocks + 7u) / 8u;
}

static int bitmap_load(void)
{
    k_u32_t bytes = bitmap_bytes();
    k_u32_t blocks = (bytes + MFS_BLOCK_SIZE - 1u) / MFS_BLOCK_SIZE;
    k_u32_t i;

    if (blocks > sizeof(g.bitmap) / MFS_BLOCK_SIZE) {
        return MFS_ENOSPC;
    }

    memset(g.bitmap, 0, sizeof(g.bitmap));

    for (i = 0; i < blocks; i++) {
        int rc = dev_read(g.sb.meta_start + i, g.bitmap + i * MFS_BLOCK_SIZE);

        if (rc != MFS_OK) {
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
        int rc = dev_write(g.sb.meta_start + i, g.bitmap + i * MFS_BLOCK_SIZE);

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

static k_u32_t inode_table_block(k_u32_t index)
{
    k_u32_t offset = index * MFS_INODE_SIZE;

    return g.sb.meta_start + g.bitmap_blocks + offset / MFS_BLOCK_SIZE;
}

static k_u32_t inode_table_offset(k_u32_t index)
{
    return (index * MFS_INODE_SIZE) % MFS_BLOCK_SIZE;
}

static int inode_read(k_u32_t index, struct mfs_inode *out)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t offset;
    int rc;

    if (index == 0 || index > g.inode_capacity) {
        return MFS_EINVAL;
    }

    rc = dev_read(inode_table_block(index), block);
    if (rc != MFS_OK) {
        return rc;
    }

    offset = inode_table_offset(index);
    memcpy(out, block + offset, sizeof(*out));

    return MFS_OK;
}

static int inode_write(k_u32_t index, const struct mfs_inode *inode)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t offset;
    int rc;

    if (index == 0 || index > g.inode_capacity) {
        return MFS_EINVAL;
    }

    rc = dev_read(inode_table_block(index), block);
    if (rc != MFS_OK) {
        return rc;
    }

    offset = inode_table_offset(index);
    memcpy(block + offset, inode, sizeof(*inode));

    return dev_write(inode_table_block(index), block);
}

/* Find a free record of the table of the inodes. */
static int inode_alloc(k_u32_t *index)
{
    k_u32_t i;

    for (i = 1; i <= g.inode_capacity; i++) {
        struct mfs_inode inode;
        int rc = inode_read(i, &inode);

        if (rc != MFS_OK) {
            return rc;
        }

        if ((inode.flags & MFS_INODE_USED) == 0) {
            *index = i;
            return MFS_OK;
        }
    }

    return MFS_ENOSPC;
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

/* The block of the chain that holds the given offset of the data. */
static int chain_block_at(k_u32_t first, k_u32_t data_offset, k_u32_t *block, k_u32_t *in_block)
{
    k_u32_t current = first;
    k_u32_t index = data_offset / MFS_BLOCK_DATA;
    k_u32_t i;

    for (i = 0; i < index; i++) {
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

    *block = current;
    *in_block = data_offset % MFS_BLOCK_DATA;

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
static int dir_write(const struct mfs_inode *inode, const struct dir_buffer *buf)
{
    k_u32_t block = inode->first_block;
    k_u32_t offset = 0;
    k_u32_t guard = 0;

    while (offset < buf->size) {
        k_size_t chunk = buf->size - offset;
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

        memcpy(block_data + 4, buf->data + offset, chunk);
        rc = dev_write(block, block_data);
        if (rc != MFS_OK) {
            return rc;
        }

        offset += (k_u32_t)chunk;

        if (offset < buf->size) {
            k_u32_t next;
            k_u32_t new_block;

            rc = chain_next(block, &next);
            if (rc != MFS_OK) {
                return rc;
            }

            if (next == MFS_NEXT_NONE) {
                rc = block_alloc(&new_block);
                if (rc != MFS_OK) {
                    return rc;
                }
                rc = chain_set_next(block, new_block);
                if (rc != MFS_OK) {
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

            if (inode_read(entry.inode, &child) == MFS_OK) {
                entry.size = child.size;
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

    if (buf->size + need > sizeof(buf->data)) {
        return MFS_E2BIG;
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

int mfs_format(struct mfs_device *dev, k_u32_t max_records)
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
    k_u32_t attempt;
    k_u32_t i;

    if (dev == NULL || dev->read_block == NULL || dev->write_block == NULL) {
        return MFS_EINVAL;
    }

    if (dev->blocks < 16) {
        return MFS_EINVAL;
    }

    if (max_records == 0) {
        max_records = 64;
    }

    total = dev->blocks;

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

    /* The first guess: four percent of the volume for the META section. */
    meta_blocks = (info_start * 4u + 99u) / 100u;
    if (meta_blocks < 2) {
        meta_blocks = 2;
    }

    /*
     * The META section has to hold the bitmap of the free blocks and the
     * table of the inodes. The bitmap depends on the size of the DATA
     * section, which depends on the size of the META section, so a few
     * rounds are enough to find the layout.
     */
    for (attempt = 0; attempt < 4; attempt++) {
        if (info_start <= meta_blocks) {
            return MFS_ENOSPC;
        }

        data_blocks = info_start - meta_blocks;
        bitmap_blocks = (data_blocks + 8u * MFS_BLOCK_SIZE - 1u) / (8u * MFS_BLOCK_SIZE);
        inode_blocks = (max_records * MFS_INODE_SIZE + MFS_BLOCK_SIZE - 1u) / MFS_BLOCK_SIZE;

        /* The bitmap lives in the memory of the manager, it has a size. */
        if (bitmap_blocks > (k_u32_t)(sizeof(g.bitmap) / MFS_BLOCK_SIZE)) {
            return MFS_ENOSPC;
        }

        if (bitmap_blocks + inode_blocks <= meta_blocks) {
            break;
        }

        meta_blocks = bitmap_blocks + inode_blocks;
    }

    if (info_start <= meta_blocks) {
        return MFS_ENOSPC;
    }

    data_blocks = info_start - meta_blocks;
    bitmap_blocks = (data_blocks + 8u * MFS_BLOCK_SIZE - 1u) / (8u * MFS_BLOCK_SIZE);
    inode_blocks = (max_records * MFS_INODE_SIZE + MFS_BLOCK_SIZE - 1u) / MFS_BLOCK_SIZE;

    if (bitmap_blocks + inode_blocks > meta_blocks) {
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
    sb.features = MFS_FEATURE_CRC;
    sb.crc = crc32(&sb, sizeof(sb) - sizeof(sb.crc));

    /* The state is needed by the helpers below. */
    memset(&g, 0, sizeof(g));
    g.dev = *dev;
    g.sb = sb;
    g.bitmap_blocks = bitmap_blocks;
    g.inode_blocks = inode_blocks;
    /* The record 0 is not used, the inodes start at 1. */
    g.inode_capacity = (inode_blocks * MFS_BLOCK_SIZE) / MFS_INODE_SIZE - 1u;

    /*
     * The bitmap: a bit of zero means that the block is free. The bits of
     * the blocks that do not exist are set, so that they are never given
     * out.
     */
    memset(g.bitmap, 0, sizeof(g.bitmap));
    if (data_blocks % 8u != 0u) {
        g.bitmap[bitmap_bytes() - 1] = (k_u8_t) ~((1u << (data_blocks % 8u)) - 1u);
    }

    /*
     * The table of the inodes is empty. The whole rest of the META section
     * belongs to the table, so that the mount finds the same number of the
     * records as the format.
     */
    {
        k_u8_t empty[MFS_BLOCK_SIZE];

        memset(empty, 0, sizeof(empty));
        for (i = 0; i < sb.meta_blocks - bitmap_blocks; i++) {
            if (dev->write_block(dev->ctx, sb.meta_start + bitmap_blocks + i, empty) != 0) {
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

        if (dev->write_block(dev->ctx, 0, block) != 0) {
            return MFS_EIO;
        }
    }

    bitmap_set(0, true);

    memset(&inode, 0, sizeof(inode));
    inode.flags = MFS_INODE_USED | MFS_INODE_DIR;
    inode.first_block = 0;
    inode.size = 0;
    inode.entries = 0;

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

        if (dev->write_block(dev->ctx, sb.info_start + sb.info_blocks - 1u, info_block) != 0) {
            return MFS_EIO;
        }
    }

    /*
     * The changes have to reach the device: the flash keeps the data in a
     * sector buffer until the filesystem says that it is done.
     */
    if (g.dev.sync != NULL) {
        return g.dev.sync(g.dev.ctx);
    }

    return MFS_OK;
}

int mfs_mount(struct mfs_device *dev)
{
    struct mfs_superblock sb;
    int rc;

    if (dev == NULL || dev->read_block == NULL) {
        return MFS_EINVAL;
    }

    memset(&g, 0, sizeof(g));
    g.dev = *dev;

    {
        k_u8_t info_block[MFS_BLOCK_SIZE];

        rc = dev->read_block(dev->ctx, dev->blocks - 1u, info_block);
        if (rc != 0) {
            return MFS_EIO;
        }

        memcpy(&sb, info_block, sizeof(sb));
    }

    if (sb.magic != MFS_MAGIC) {
        return MFS_EINVAL;
    }

    if (sb.version != MFS_VERSION || sb.block_size != MFS_BLOCK_SIZE) {
        return MFS_EINVAL;
    }

    if (sb.total_blocks != dev->blocks) {
        return MFS_EINVAL;
    }

    /* A header that a brownout left half written must not be trusted. */
    if ((sb.features & MFS_FEATURE_CRC) != 0u) {
        if (sb.crc != crc32(&sb, sizeof(sb) - sizeof(sb.crc))) {
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

    g.sb = sb;

    rc = bitmap_load();
    if (rc != MFS_OK) {
        return rc;
    }

    /* The rest of the META section belongs to the table of the inodes. */
    if (sb.meta_blocks <= g.bitmap_blocks) {
        return MFS_EINVAL;
    }

    g.inode_blocks = sb.meta_blocks - g.bitmap_blocks;
    /* The record 0 is not used, the inodes start at 1. */
    g.inode_capacity = (g.inode_blocks * MFS_BLOCK_SIZE) / MFS_INODE_SIZE - 1u;

    if (g.inode_capacity < MFS_ROOT_INODE) {
        return MFS_EINVAL;
    }

    g.mounted = true;

    return MFS_OK;
}

void mfs_unmount(void)
{
    memset(&g, 0, sizeof(g));
}

int mfs_sync(void)
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

    /* The changes have to reach the device: the flash keeps the data in a
     * sector buffer until the filesystem says that it is done. */
    if (g.dev.sync != NULL) {
        return g.dev.sync(g.dev.ctx);
    }

    return MFS_OK;
}

bool mfs_is_mounted(void)
{
    return g.mounted;
}

int mfs_get_info(struct mfs_info *out)
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

        if (inode_read(i, &inode) != MFS_OK) {
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
    rc = dir_write(&parent, dir);
    if (rc != MFS_OK) {
        return rc;
    }

    return inode_write(dir_inode, &parent);
}

int mfs_mkdir(const char *path)
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
            block_release(block);
            return rc;
        }
    }

    memset(&inode, 0, sizeof(inode));
    inode.flags = MFS_INODE_USED | MFS_INODE_DIR;
    inode.first_block = block;
    inode.size = 0;
    inode.entries = 0;

    rc = inode_write(index, &inode);
    if (rc != MFS_OK) {
        block_release(block);
        return rc;
    }

    rc = add_to_parent(dir_path, name, index, (k_u8_t)inode.flags);
    if (rc != MFS_OK) {
        memset(&inode, 0, sizeof(inode));
        inode_write(index, &inode);
        block_release(block);
        return rc;
    }

    /* The new directory has to survive the restart. */
    return mfs_sync();
}

int mfs_create(const char *path)
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

    rc = inode_alloc(&index);
    if (rc != MFS_OK) {
        return rc;
    }

    memset(&inode, 0, sizeof(inode));
    inode.flags = MFS_INODE_USED;
    inode.first_block = MFS_NEXT_NONE;
    inode.size = 0;
    inode.entries = 0;

    rc = inode_write(index, &inode);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = add_to_parent(dir_path, name, index, (k_u8_t)inode.flags);
    if (rc != MFS_OK) {
        memset(&inode, 0, sizeof(inode));
        inode_write(index, &inode);
        return rc;
    }

    /* The new object has to survive the restart. */
    return mfs_sync();
}

int mfs_open(const char *path, int flags, struct mfs_file *out)
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
        return MFS_EISDIR;
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
    out->inode = entry.inode;
    out->first_block = inode.first_block;
    out->size = inode.size;
    out->offset = 0;
    out->flags = flags;

    return MFS_OK;
}

int mfs_read(struct mfs_file *file, void *buf, k_size_t size)
{
    k_u8_t *out = (k_u8_t *)buf;
    k_size_t done = 0;
    int rc;

    if (file == NULL || buf == NULL) {
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

        rc = chain_block_at(file->first_block, file->offset, &block, &in_block);
        if (rc != MFS_OK) {
            return rc;
        }

        chunk = MFS_BLOCK_DATA - in_block;
        if (chunk > size - done) {
            chunk = size - done;
        }

        {
            k_u8_t block_data[MFS_BLOCK_SIZE];

            rc = dev_read(block, block_data);
            if (rc != MFS_OK) {
                return rc;
            }
            memcpy(out + done, block_data + 4 + in_block, chunk);
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
        /* The first block of the file. */
        file->first_block = block;
        *new_block = block;
        return MFS_OK;
    }

    rc = chain_last(file->first_block, &last);
    if (rc != MFS_OK) {
        block_release(block);
        return rc;
    }

    rc = chain_set_next(last, block);
    if (rc != MFS_OK) {
        block_release(block);
        return rc;
    }

    *new_block = block;
    return MFS_OK;
}

int mfs_write(struct mfs_file *file, const void *buf, k_size_t size)
{
    const k_u8_t *in = (const k_u8_t *)buf;
    k_size_t done = 0;
    k_u32_t old_blocks = 0;
    int rc;

    if (file == NULL || buf == NULL) {
        return MFS_EINVAL;
    }

    if ((file->flags & (MFS_O_WRONLY | MFS_O_RDWR)) == 0) {
        return MFS_EINVAL;
    }

    /* The file has to have enough blocks for the whole write. */
    {
        k_u32_t have = 0;
        k_u32_t needed = (file->offset + size + MFS_BLOCK_DATA - 1u) / MFS_BLOCK_DATA;

        if (file->first_block != MFS_NEXT_NONE) {
            rc = chain_count(file->first_block, &have);
            if (rc != MFS_OK) {
                return rc;
            }
        }

        old_blocks = have;

        while (have < needed) {
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
            have++;
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

        rc = chain_block_at(file->first_block, file->offset, &block, &in_block);
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

int mfs_close(struct mfs_file *file)
{
    struct mfs_inode inode;
    int rc;

    if (file == NULL) {
        return MFS_EINVAL;
    }

    if (file->dirty) {
        rc = inode_read(file->inode, &inode);
        if (rc != MFS_OK) {
            return rc;
        }
        inode.size = file->size;
        inode.first_block = file->first_block;
        rc = inode_write(file->inode, &inode);
        if (rc != MFS_OK) {
            return rc;
        }
        file->dirty = false;
    }

    /* The blocks of the file have been taken, the map has to know it. */
    return mfs_sync();
}

int mfs_unlink(const char *path)
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

        rc = dir_read(&inode, content);
        if (rc != MFS_OK) {
            return rc;
        }
        if (content->size > 0) {
            return MFS_ENOTEMPTY;
        }
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

int mfs_stat(const char *path, struct mfs_inode *out, struct mfs_dirent *dirent)
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

    if (out != NULL) {
        *out = inode;
    }

    return MFS_OK;
}

int mfs_list(const char *path, struct mfs_dirent *out, k_size_t max)
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
    default:
        return "unknown error";
    }
}
