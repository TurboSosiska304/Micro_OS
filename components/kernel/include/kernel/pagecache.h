/*
 * Micro OS — the page cache.
 *
 * A file is read and written in pages of PAGECACHE_PAGE_SIZE bytes. The
 * cache keeps one copy of a page for every task: the key is the mount, the
 * inode and the page offset. A miss reads the page through the filesystem,
 * a write marks the page dirty, and a sync or an eviction writes it back.
 *
 * The cache is a bounded pool: when it is full the least recently used
 * clean page is dropped, and a dirty one is written back before it leaves.
 * The size is PAGECACHE_MAX_BYTES, 128 KiB by default so the host test heap is
 * not starved; a 32 MiB machine raises it through the build config.
 */
#ifndef MICRO_OS_KERNEL_PAGECACHE_H
#define MICRO_OS_KERNEL_PAGECACHE_H

#include "kernel/types.h"
#include "kernel/vfs.h"

#define PAGECACHE_PAGE_SIZE 4096u

#ifndef PAGECACHE_MAX_BYTES
#define PAGECACHE_MAX_BYTES (128u * 1024u)
#endif

struct pagecache_stats {
    k_u64_t hits;
    k_u64_t misses;
    k_u64_t dirty;     /* dirty pages currently in the cache */
    k_u64_t evictions; /* pages evicted to make room */
    k_u64_t pages;     /* pages currently in the cache */
    k_u64_t capacity;  /* the most pages the cache can hold */
};

/* Prepare the cache. */
void pagecache_init(void);

/* Drop every page and give the memory back. */
void pagecache_reset(void);

/*
 * Set the maximum number of pages the cache may hold. It resets the cache,
 * so it is meant for tests and for the boot configuration.
 */
void pagecache_set_capacity(k_u32_t pages);

/*
 * Read `size` bytes at `file->offset` through the cache. The filesystem
 * fills a missing page with its readpage operation. Returns the bytes read
 * or a negative error; `file->offset` and `file->size` are updated.
 */
k_ssize_t pagecache_read(struct vfs_file *file, void *buf, k_size_t size);

/*
 * Write `size` bytes at `file->offset` through the cache. A partial page
 * that is not in the cache is read first; the page is marked dirty and not
 * written to the medium until a sync or an eviction. Returns the bytes
 * written or a negative error.
 */
k_ssize_t pagecache_write(struct vfs_file *file, const void *buf, k_size_t size);

/* Write the dirty pages of one file back through writepage and then sync. */
int pagecache_sync_file(struct vfs_file *file);

/* Write every dirty page of the cache back. */
int pagecache_sync_all(void);

/* Drop every page but keep the cache ready. */
void pagecache_drop_all(void);

/* Forget the cached pages of one inode of a mount. */
void pagecache_invalidate(void *fs_key, k_u32_t inode);

/* Forget every cached page of one mount. */
void pagecache_invalidate_fs(void *fs_key);

/*
 * Adjust the cache after a file was truncated to `new_size`: pages beyond
 * the new end are dropped, and a partial last page is zeroed past the end.
 */
void pagecache_truncate(void *fs_key, k_u32_t inode, k_u64_t new_size);

/*
 * Return the file size known to the cache: the on-disk size, or the end of
 * the furthest cached page when dirty writes have extended the file.
 */
k_u64_t pagecache_inode_size(void *fs_key, k_u32_t inode, k_u64_t disk_size);

void pagecache_get_stats(struct pagecache_stats *out);

/*
 * The key of the mount for a file: the context of the filesystem, or the
 * operations when the filesystem has no context (MicroFS is a singleton).
 */
static inline void *pagecache_fs_key(const struct vfs_file *file)
{
    if (file == NULL) {
        return NULL;
    }

    return (file->fs != NULL) ? file->fs : (void *)file->ops;
}

#endif /* MICRO_OS_KERNEL_PAGECACHE_H */
