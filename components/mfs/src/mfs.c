#include "mfs/mfs.h"

#include <string.h>

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
    k_u32_t bitmap_blocks;  /* the blocks of the bitmap of the free blocks */
    k_u32_t inode_blocks;   /* the blocks of the table of the inodes */
    k_u32_t inode_capacity; /* the number of the records of the table */
    bool bitmap_dirty;      /* the map in the memory is newer than the device */
    k_u8_t bitmap[MFS_BLOCK_SIZE * MFS_BITMAP_MAX_BLOCKS];
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
            entry.mode = 0;
            entry.owner = 0;

            if (inode_read(entry.inode, &child) == MFS_OK) {
                entry.size = child.size;
                entry.mode = mfs_inode_mode(&child);
                entry.owner = mfs_inode_owner(&child);
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
    k_u32_t attempt;
    k_u32_t i;

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
    g.dev = dev;
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
            if (dev_write(sb.meta_start + bitmap_blocks + i, empty) != MFS_OK) {
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

    if (sb.version != MFS_VERSION || sb.block_size != MFS_BLOCK_SIZE) {
        return MFS_EINVAL;
    }

    if (sb.total_blocks != dev->sectors) {
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

    /* The root of the volume is a directory, and the header says it is the
     * record 1. A volume whose header is intact but whose table of the
     * inodes is not would mount and then fail every lookup: better to say
     * that the volume is damaged and keep the data for a person. */
    if (sb.root_inode != MFS_ROOT_INODE || (sb.info_start % 4u) != 0u) {
        kerror("mfs: the header names a root that cannot exist");
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

    {
        struct mfs_inode root;

        if (inode_read(MFS_ROOT_INODE, &root) != MFS_OK) {
            return MFS_EINVAL;
        }

        if ((root.flags & (MFS_INODE_USED | MFS_INODE_DIR)) != (MFS_INODE_USED | MFS_INODE_DIR) ||
            root.first_block >= sb.data_blocks) {
            kerror("mfs: the root of the volume is not a directory (flags 0x%x)",
                   (unsigned)root.flags);
            return MFS_EINVAL;
        }
    }

    g.mounted = true;

    return MFS_OK;
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
    inode.flags = inode_flags(MFS_INODE_USED | MFS_INODE_DIR, MFS_MODE_DIR_DEFAULT, 0);
    inode.first_block = block;
    inode.size = 0;
    inode.entries = 0;

    rc = inode_write(index, &inode);
    if (rc != MFS_OK) {
        block_release(block);
        return rc;
    }

    rc = add_to_parent(dir_path, name, index, (k_u8_t)(inode.flags & MFS_INODE_TYPE_MASK));
    if (rc != MFS_OK) {
        memset(&inode, 0, sizeof(inode));
        inode_write(index, &inode);
        block_release(block);
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

    rc = inode_alloc(&index);
    if (rc != MFS_OK) {
        return rc;
    }

    memset(&inode, 0, sizeof(inode));
    inode.flags = inode_flags(MFS_INODE_USED, MFS_MODE_DEFAULT, 0);
    inode.first_block = MFS_NEXT_NONE;
    inode.size = 0;
    inode.entries = 0;

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

static int mfs_write_locked(struct mfs_file *file, const void *buf, k_size_t size)
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

static int mfs_close_locked(struct mfs_file *file)
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

static int mfs_unlink_locked(const char *path)
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

/*
 * A change of the mode or of the owner is a change of the record of the
 * object: the new flags are written into the table and the volume is
 * synced, so that the numbers survive the restart like the size of a
 * file does.
 */
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
 * The public functions of the filesystem hold the lock of the volume
 * for the whole of their work: the volume is one piece of state and
 * two tasks that touch it together corrupt it.
 */
int mfs_format(const struct k_blockdev *dev, k_u32_t max_records)
{
    int rc;

    mfs_lock();
    rc = mfs_format_locked(dev, max_records);
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
    memset(&view, 0, sizeof(view));
    view.sb = &g.sb;
    view.bitmap = g.bitmap;
    view.dir_scratch = g_dir_buffer.data;
    view.dir_scratch_size = sizeof(g_dir_buffer.data);
    view.bitmap_blocks = g.bitmap_blocks;
    view.inode_blocks = g.inode_blocks;
    view.inode_capacity = g.inode_capacity;
    view.device_blocks = g.dev->sectors;

    rc = mfs_check_locked(&view, report);

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

int mfs_close(struct mfs_file *file)
{
    int rc;

    mfs_lock();
    rc = mfs_close_locked(file);
    mfs_unlock();

    return rc;
}

int mfs_unlink(const char *path)
{
    int rc;

    mfs_lock();
    rc = mfs_unlink_locked(path);
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

int mfs_list(const char *path, struct mfs_dirent *out, k_size_t max)
{
    int rc;

    mfs_lock();
    rc = mfs_list_locked(path, out, max);
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
    default:
        return "unknown error";
    }
}
