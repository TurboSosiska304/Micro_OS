#include "kernel/pagecache.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/lock.h"
#include "kernel/log.h"

#define PC_PAGE_SIZE PAGECACHE_PAGE_SIZE

struct pagecache_page {
    void *fs_key;
    void *fs;
    const struct vfs_operations *ops;
    k_u32_t inode;
    k_u32_t page_index;
    k_u32_t valid; /* bytes of file data in this page, 0..PAGECACHE_PAGE_SIZE */
    k_u16_t flags;
    k_u16_t refcount;
    struct pagecache_page *hash_next;
    struct pagecache_page *lru_prev;
    struct pagecache_page *lru_next;
    k_u8_t *data;
};

#define PC_VALID 0x0001u
#define PC_DIRTY 0x0002u
#define PC_BUSY 0x0004u
#define PC_DEAD 0x0008u

static struct pagecache_page **g_buckets;
static k_u32_t g_bucket_count;
static struct pagecache_page g_lru_head; /* sentinel: next=MRU, prev=LRU */
static k_u32_t g_pages;
static k_u32_t g_capacity;
static k_u64_t g_hits;
static k_u64_t g_misses;
static k_u64_t g_evictions;
static k_mutex_t g_lock = K_MUTEX_INITIALIZER;
static bool g_ready;

static k_u32_t hash_key(void *fs_key, k_u32_t inode, k_u32_t page_index)
{
    k_u32_t h = (k_u32_t)(uintptr_t)fs_key;
    h ^= inode * 2654435761u;
    h ^= page_index * 2246822519u;
    return h;
}

static void lru_remove(struct pagecache_page *p)
{
    p->lru_prev->lru_next = p->lru_next;
    p->lru_next->lru_prev = p->lru_prev;
    p->lru_prev = NULL;
    p->lru_next = NULL;
}

static void lru_push_front(struct pagecache_page *p)
{
    p->lru_next = g_lru_head.lru_next;
    p->lru_prev = &g_lru_head;
    g_lru_head.lru_next->lru_prev = p;
    g_lru_head.lru_next = p;
}

static void lru_move_front(struct pagecache_page *p)
{
    if (p->lru_prev == NULL) {
        return;
    }
    lru_remove(p);
    lru_push_front(p);
}

static struct pagecache_page *bucket_find(void *fs_key, k_u32_t inode, k_u32_t page_index,
                                          struct pagecache_page ***pprev)
{
    k_u32_t b = hash_key(fs_key, inode, page_index) & (g_bucket_count - 1u);
    struct pagecache_page **prev = &g_buckets[b];
    struct pagecache_page *p = *prev;

    while (p != NULL) {
        if (p->fs_key == fs_key && p->inode == inode && p->page_index == page_index) {
            if (pprev != NULL) {
                *pprev = prev;
            }
            return p;
        }
        prev = &p->hash_next;
        p = *prev;
    }

    if (pprev != NULL) {
        *pprev = prev;
    }
    return NULL;
}

static void bucket_insert(struct pagecache_page *p)
{
    k_u32_t b = hash_key(p->fs_key, p->inode, p->page_index) & (g_bucket_count - 1u);
    p->hash_next = g_buckets[b];
    g_buckets[b] = p;
}

static void bucket_remove(struct pagecache_page *p)
{
    struct pagecache_page **prev;
    struct pagecache_page *found = bucket_find(p->fs_key, p->inode, p->page_index, &prev);
    if (found == p) {
        *prev = p->hash_next;
        p->hash_next = NULL;
    }
}

static k_u32_t count_dirty_locked(void)
{
    struct pagecache_page *p = g_lru_head.lru_next;
    k_u32_t n = 0;

    while (p != &g_lru_head) {
        if ((p->flags & PC_DIRTY) != 0u) {
            n++;
        }
        p = p->lru_next;
    }
    return n;
}

static k_u32_t next_pow2(k_u32_t v)
{
    k_u32_t p = 1;

    while (p < v && p != 0u) {
        p <<= 1;
    }
    return p == 0u ? v : p;
}

static void page_free(struct pagecache_page *p)
{
    if (p == NULL) {
        return;
    }
    if (p->data != NULL) {
        kmm_free(p->data);
    }
    kmm_free(p);
}

void pagecache_init(void)
{
    if (k_mutex_lock(&g_lock) != 0) {
        return;
    }

    if (g_ready) {
        k_mutex_unlock(&g_lock);
        return;
    }

    g_capacity = (k_u32_t)(PAGECACHE_MAX_BYTES / PC_PAGE_SIZE);
    if (g_capacity == 0u) {
        g_capacity = 1u;
    }

    g_bucket_count = next_pow2(g_capacity);
    if (g_bucket_count < 16u) {
        g_bucket_count = 16u;
    }

    g_buckets = (struct pagecache_page **)kmm_alloc(
        (k_size_t)g_bucket_count * sizeof(struct pagecache_page *), KMM_PRIORITY_KERNEL);
    if (g_buckets == NULL) {
        g_capacity = 0;
        g_bucket_count = 0;
        k_mutex_unlock(&g_lock);
        kerror("pagecache: no memory for buckets");
        return;
    }

    memset(g_buckets, 0, (k_size_t)g_bucket_count * sizeof(struct pagecache_page *));
    g_lru_head.lru_next = &g_lru_head;
    g_lru_head.lru_prev = &g_lru_head;
    g_pages = 0;
    g_hits = 0;
    g_misses = 0;
    g_evictions = 0;
    g_ready = true;

    k_mutex_unlock(&g_lock);
    kinfo("pagecache: %u pages, %u KiB", (unsigned)g_capacity,
          (unsigned)((k_u64_t)g_capacity * PC_PAGE_SIZE / 1024u));
}

static int page_writeback(struct pagecache_page *p)
{
    k_u64_t offset;
    int rc;

    if (p == NULL || p->ops == NULL || p->ops->writepage == NULL) {
        return KERR_INVAL;
    }

    if (p->valid == 0u) {
        return 0;
    }

    offset = (k_u64_t)p->page_index * PC_PAGE_SIZE;
    rc = p->ops->writepage(p->fs, p->inode, offset, p->data, p->valid);
    if (rc < 0) {
        return rc;
    }
    if ((k_u32_t)rc != p->valid) {
        return KERR_IO;
    }

    return 0;
}

static void page_detach(struct pagecache_page *p)
{
    bucket_remove(p);
    if (p->lru_prev != NULL) {
        lru_remove(p);
    }
    if (g_pages > 0u) {
        g_pages--;
    }
}

static struct pagecache_page *page_alloc(void)
{
    struct pagecache_page *p;

    p = (struct pagecache_page *)kmm_alloc(sizeof(*p), KMM_PRIORITY_KERNEL);
    if (p == NULL) {
        return NULL;
    }
    memset(p, 0, sizeof(*p));
    p->data = (k_u8_t *)kmm_alloc(PC_PAGE_SIZE, KMM_PRIORITY_KERNEL);
    if (p->data == NULL) {
        kmm_free(p);
        return NULL;
    }
    return p;
}

/*
 * Evict one page. Must be called with g_lock held. Returns 0 on success,
 * KERR_NOSPC if no evictable page, or a negative writeback error.
 * On success, *out_victim may be set to a detached page for reuse, or NULL.
 */
static int page_evict_one(struct pagecache_page **out_victim)
{
    struct pagecache_page *p;

    *out_victim = NULL;

    /* Prefer a clean, unpinned page. */
    for (p = g_lru_head.lru_prev; p != &g_lru_head; p = p->lru_prev) {
        if (p->refcount == 0u && (p->flags & PC_DIRTY) == 0u) {
            page_detach(p);
            g_evictions++;
            *out_victim = p;
            return 0;
        }
    }

    /* Dirty victim: write it back, then evict. */
    for (p = g_lru_head.lru_prev; p != &g_lru_head; p = p->lru_prev) {
        if (p->refcount == 0u && (p->flags & PC_DIRTY) != 0u) {
            int rc = page_writeback(p);

            if (rc != 0) {
                return rc;
            }

            p->flags &= (k_u16_t)~PC_DIRTY;
            page_detach(p);
            g_evictions++;
            *out_victim = p;
            return 0;
        }
    }

    return KERR_NOSPC;
}

static struct pagecache_page *page_get_locked(void *fs_key, k_u32_t inode, k_u32_t page_index)
{
    return bucket_find(fs_key, inode, page_index, NULL);
}

static struct pagecache_page *page_lookup_or_load(struct vfs_file *file, k_u32_t page_index,
                                                  int *out_err)
{
    void *fs_key = pagecache_fs_key(file);
    struct pagecache_page *p;
    struct pagecache_page *victim = NULL;
    k_u64_t offset;
    int rc;

    *out_err = 0;
    p = page_get_locked(fs_key, file->inode, page_index);
    if (p != NULL) {
        g_hits++;
        lru_move_front(p);
        return p;
    }

    g_misses++;

    if (g_pages >= g_capacity) {
        rc = page_evict_one(&victim);
        if (rc != 0) {
            *out_err = rc;
            return NULL;
        }
    }

    if (victim == NULL) {
        victim = page_alloc();
        if (victim == NULL) {
            /* Try eviction as last resort. */
            rc = page_evict_one(&victim);
            if (rc != 0 || victim == NULL) {
                *out_err = (rc != 0) ? rc : KERR_NOMEM;
                return NULL;
            }
        }
    }

    memset(victim->data, 0, PC_PAGE_SIZE);
    victim->fs_key = fs_key;
    victim->fs = file->fs;
    victim->ops = file->ops;
    victim->inode = file->inode;
    victim->page_index = page_index;
    victim->valid = 0;
    victim->flags = 0;
    victim->refcount = 0;
    victim->hash_next = NULL;

    offset = (k_u64_t)page_index * PC_PAGE_SIZE;
    if (file->ops != NULL && file->ops->readpage != NULL) {
        rc = file->ops->readpage(file->fs, file->inode, offset, victim->data, PC_PAGE_SIZE);
        if (rc < 0) {
            page_free(victim);
            *out_err = rc;
            return NULL;
        }
        victim->valid = (k_u32_t)rc;
    } else {
        page_free(victim);
        *out_err = KERR_NOTSUP;
        return NULL;
    }

    victim->flags = PC_VALID;
    bucket_insert(victim);
    lru_push_front(victim);
    g_pages++;
    return victim;
}

k_ssize_t pagecache_read(struct vfs_file *file, void *buf, k_size_t size)
{
    k_u8_t *out = (k_u8_t *)buf;
    k_size_t done = 0;

    if (file == NULL || buf == NULL || file->ops == NULL || file->ops->readpage == NULL) {
        return KERR_INVAL;
    }

    if (k_mutex_lock(&g_lock) != 0) {
        return KERR_NOMEM;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        pagecache_init();
        if (k_mutex_lock(&g_lock) != 0) {
            return KERR_NOMEM;
        }
        if (!g_ready) {
            k_mutex_unlock(&g_lock);
            return KERR_NOMEM;
        }
    }

    while (done < size) {
        k_u64_t off = file->offset;
        k_u32_t page_index;
        k_u32_t in_page;
        k_size_t chunk;
        struct pagecache_page *p;
        int err = 0;
        k_u64_t page_end;

        page_index = (k_u32_t)(off / PC_PAGE_SIZE);
        in_page = (k_u32_t)(off % PC_PAGE_SIZE);

        p = page_lookup_or_load(file, page_index, &err);
        if (p == NULL) {
            k_mutex_unlock(&g_lock);
            return (done > 0) ? (k_ssize_t)done : err;
        }

        page_end = (k_u64_t)page_index * PC_PAGE_SIZE + p->valid;
        if (p->valid < PC_PAGE_SIZE && file->size < page_end) {
            file->size = page_end;
        } else if (p->valid < PC_PAGE_SIZE && file->size > page_end) {
            k_size_t want = (k_size_t)(file->size - (k_u64_t)page_index * PC_PAGE_SIZE);
            if (want > PC_PAGE_SIZE) {
                want = PC_PAGE_SIZE;
            }
            if (want > p->valid) {
                memset(p->data + p->valid, 0, want - p->valid);
                p->valid = want;
                page_end = (k_u64_t)page_index * PC_PAGE_SIZE + p->valid;
            }
        }

        if (off >= file->size) {
            break;
        }

        if (in_page >= p->valid) {
            break;
        }

        chunk = p->valid - in_page;
        if (chunk > size - done) {
            chunk = size - done;
        }
        if (file->size > off && off + chunk > file->size) {
            chunk = (k_size_t)(file->size - off);
        }
        if (chunk == 0u) {
            break;
        }

        memcpy(out + done, p->data + in_page, chunk);
        done += chunk;
        file->offset += (k_u64_t)chunk;
    }

    k_mutex_unlock(&g_lock);
    return (k_ssize_t)done;
}

k_ssize_t pagecache_write(struct vfs_file *file, const void *buf, k_size_t size)
{
    const k_u8_t *in = (const k_u8_t *)buf;
    k_size_t done = 0;

    if (file == NULL || buf == NULL || file->ops == NULL || file->ops->readpage == NULL ||
        file->ops->writepage == NULL) {
        return KERR_INVAL;
    }

    if (k_mutex_lock(&g_lock) != 0) {
        return KERR_NOMEM;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        pagecache_init();
        if (k_mutex_lock(&g_lock) != 0) {
            return KERR_NOMEM;
        }
        if (!g_ready) {
            k_mutex_unlock(&g_lock);
            return KERR_NOMEM;
        }
    }

    while (done < size) {
        k_u64_t off = file->offset;
        k_u32_t page_index = (k_u32_t)(off / PC_PAGE_SIZE);
        k_u32_t in_page = (k_u32_t)(off % PC_PAGE_SIZE);
        k_size_t chunk = PC_PAGE_SIZE - in_page;
        struct pagecache_page *p;
        int err = 0;
        bool need_read;

        if (chunk > size - done) {
            chunk = size - done;
        }

        need_read = (in_page != 0u) || (chunk < PC_PAGE_SIZE);
        /* If the write starts beyond known file size in this page and we don't
         * cover the whole page, we need the old page to preserve/zero gap. */
        if (!need_read) {
            /* Whole page write - no read needed. */
        }

        p = page_get_locked(pagecache_fs_key(file), file->inode, page_index);
        if (p == NULL) {
            if (need_read) {
                p = page_lookup_or_load(file, page_index, &err);
                if (p == NULL) {
                    k_mutex_unlock(&g_lock);
                    return (done > 0) ? (k_ssize_t)done : err;
                }
            } else {
                /* Allocate new page without reading. */
                struct pagecache_page *victim = NULL;
                int rc;

                g_misses++;
                if (g_pages >= g_capacity) {
                    rc = page_evict_one(&victim);
                    if (rc != 0) {
                        k_mutex_unlock(&g_lock);
                        return (done > 0) ? (k_ssize_t)done : rc;
                    }
                }
                if (victim == NULL) {
                    victim = page_alloc();
                    if (victim == NULL) {
                        rc = page_evict_one(&victim);
                        if (rc != 0 || victim == NULL) {
                            k_mutex_unlock(&g_lock);
                            return (done > 0) ? (k_ssize_t)done : ((rc != 0) ? rc : KERR_NOMEM);
                        }
                    }
                }
                memset(victim->data, 0, PC_PAGE_SIZE);
                victim->fs_key = pagecache_fs_key(file);
                victim->fs = file->fs;
                victim->ops = file->ops;
                victim->inode = file->inode;
                victim->page_index = page_index;
                victim->valid = 0;
                victim->flags = PC_VALID;
                victim->refcount = 0;
                victim->hash_next = NULL;
                bucket_insert(victim);
                lru_push_front(victim);
                g_pages++;
                p = victim;
            }
        } else {
            g_hits++;
            lru_move_front(p);
        }

        /* Zero-fill gap between old valid and write start within page if extending. */
        if (in_page > p->valid) {
            memset(p->data + p->valid, 0, in_page - p->valid);
        }

        memcpy(p->data + in_page, in + done, chunk);
        if (in_page + (k_u32_t)chunk > p->valid) {
            p->valid = in_page + (k_u32_t)chunk;
        }
        p->flags |= PC_DIRTY | PC_VALID;
        p->fs = file->fs;
        p->ops = file->ops;

        done += chunk;
        file->offset += (k_u64_t)chunk;
        if (file->offset > file->size) {
            file->size = file->offset;
        }
    }

    k_mutex_unlock(&g_lock);
    return (k_ssize_t)done;
}

int pagecache_sync_file(struct vfs_file *file)
{
    void *fs_key;
    struct pagecache_page *p;
    int rc = 0;

    if (file == NULL) {
        return KERR_INVAL;
    }

    if (k_mutex_lock(&g_lock) != 0) {
        return KERR_NOMEM;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        return 0;
    }

    fs_key = pagecache_fs_key(file);

    for (p = g_lru_head.lru_next; p != &g_lru_head; p = p->lru_next) {
        if (p->fs_key == fs_key && p->inode == file->inode && (p->flags & PC_DIRTY) != 0u) {
            int wrc = page_writeback(p);
            if (wrc != 0) {
                rc = wrc;
                break;
            }
            p->flags &= (k_u16_t)~PC_DIRTY;
        }
    }

    if (rc == 0 && file->ops != NULL && file->ops->sync != NULL) {
        rc = file->ops->sync(file->fs, file->inode);
    }

    k_mutex_unlock(&g_lock);
    return rc;
}

int pagecache_sync_all(void)
{
    struct pagecache_page *p;
    int rc = 0;

    if (k_mutex_lock(&g_lock) != 0) {
        return KERR_NOMEM;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        return 0;
    }

    for (p = g_lru_head.lru_next; p != &g_lru_head; p = p->lru_next) {
        if ((p->flags & PC_DIRTY) != 0u) {
            int wrc = page_writeback(p);
            if (wrc != 0) {
                rc = wrc;
            } else {
                p->flags &= (k_u16_t)~PC_DIRTY;
            }
        }
    }

    k_mutex_unlock(&g_lock);
    return rc;
}

void pagecache_invalidate(void *fs_key, k_u32_t inode)
{
    struct pagecache_page *p;
    struct pagecache_page *next;

    if (k_mutex_lock(&g_lock) != 0) {
        return;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        return;
    }

    p = g_lru_head.lru_next;
    while (p != &g_lru_head) {
        next = p->lru_next;
        if (p->fs_key == fs_key && p->inode == inode) {
            page_detach(p);
            page_free(p);
        }
        p = next;
    }

    k_mutex_unlock(&g_lock);
}

void pagecache_invalidate_fs(void *fs_key)
{
    struct pagecache_page *p;
    struct pagecache_page *next;

    if (k_mutex_lock(&g_lock) != 0) {
        return;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        return;
    }

    p = g_lru_head.lru_next;
    while (p != &g_lru_head) {
        next = p->lru_next;
        if (p->fs_key == fs_key) {
            page_detach(p);
            page_free(p);
        }
        p = next;
    }

    k_mutex_unlock(&g_lock);
}

void pagecache_truncate(void *fs_key, k_u32_t inode, k_u64_t new_size)
{
    struct pagecache_page *p;
    struct pagecache_page *next;
    k_u32_t last_page;
    k_u32_t last_off;

    if (k_mutex_lock(&g_lock) != 0) {
        return;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        return;
    }

    last_page = (k_u32_t)(new_size / PC_PAGE_SIZE);
    last_off = (k_u32_t)(new_size % PC_PAGE_SIZE);

    p = g_lru_head.lru_next;
    while (p != &g_lru_head) {
        next = p->lru_next;
        if (p->fs_key == fs_key && p->inode == inode) {
            if (p->page_index > last_page || (p->page_index == last_page && last_off == 0u &&
                                              new_size <= (k_u64_t)p->page_index * PC_PAGE_SIZE)) {
                page_detach(p);
                page_free(p);
            } else if (p->page_index == last_page) {
                if (last_off < p->valid) {
                    memset(p->data + last_off, 0, p->valid - last_off);
                    p->valid = last_off;
                    p->flags |= PC_DIRTY;
                }
            }
        }
        p = next;
    }

    k_mutex_unlock(&g_lock);
}

k_u64_t pagecache_inode_size(void *fs_key, k_u32_t inode, k_u64_t disk_size)
{
    struct pagecache_page *p;
    k_u64_t size = disk_size;

    if (k_mutex_lock(&g_lock) != 0) {
        return disk_size;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        return disk_size;
    }

    for (p = g_lru_head.lru_next; p != &g_lru_head; p = p->lru_next) {
        if (p->fs_key == fs_key && p->inode == inode) {
            k_u64_t end = (k_u64_t)p->page_index * PC_PAGE_SIZE + p->valid;
            if (end > size) {
                size = end;
            }
        }
    }

    k_mutex_unlock(&g_lock);
    return size;
}

void pagecache_get_stats(struct pagecache_stats *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (k_mutex_lock(&g_lock) != 0) {
        return;
    }

    out->hits = g_hits;
    out->misses = g_misses;
    out->dirty = count_dirty_locked();
    out->evictions = g_evictions;
    out->pages = g_pages;
    out->capacity = g_capacity;

    k_mutex_unlock(&g_lock);
}

void pagecache_drop_all(void)
{
    struct pagecache_page *p;
    struct pagecache_page *next;

    if (k_mutex_lock(&g_lock) != 0) {
        return;
    }

    if (!g_ready) {
        k_mutex_unlock(&g_lock);
        return;
    }

    p = g_lru_head.lru_next;
    while (p != &g_lru_head) {
        next = p->lru_next;
        if ((p->flags & PC_DIRTY) != 0u) {
            (void)page_writeback(p);
        }
        page_detach(p);
        page_free(p);
        p = next;
    }

    k_mutex_unlock(&g_lock);
}

static void pagecache_reset_locked(void)
{
    struct pagecache_page *p;
    struct pagecache_page *next;

    if (g_ready) {
        p = g_lru_head.lru_next;
        while (p != &g_lru_head) {
            next = p->lru_next;
            if ((p->flags & PC_DIRTY) != 0u) {
                (void)page_writeback(p);
            }
            page_detach(p);
            page_free(p);
            p = next;
        }

        if (g_buckets != NULL) {
            kmm_free(g_buckets);
            g_buckets = NULL;
        }
    }

    g_bucket_count = 0;
    g_pages = 0;
    g_capacity = 0;
    g_hits = 0;
    g_misses = 0;
    g_evictions = 0;
    g_lru_head.lru_next = &g_lru_head;
    g_lru_head.lru_prev = &g_lru_head;
    g_ready = false;
}

void pagecache_reset(void)
{
    if (k_mutex_lock(&g_lock) != 0) {
        return;
    }
    pagecache_reset_locked();
    k_mutex_unlock(&g_lock);
}

void pagecache_set_capacity(k_u32_t pages)
{
    if (pages == 0u) {
        pages = 1u;
    }

    if (k_mutex_lock(&g_lock) != 0) {
        return;
    }

    pagecache_reset_locked();
    g_capacity = pages;
    g_bucket_count = next_pow2(pages);
    if (g_bucket_count < 16u) {
        g_bucket_count = 16u;
    }
    g_buckets = (struct pagecache_page **)kmm_alloc(
        (k_size_t)g_bucket_count * sizeof(struct pagecache_page *), KMM_PRIORITY_KERNEL);
    if (g_buckets == NULL) {
        g_capacity = 0;
        g_bucket_count = 0;
        k_mutex_unlock(&g_lock);
        return;
    }
    memset(g_buckets, 0, (k_size_t)g_bucket_count * sizeof(struct pagecache_page *));
    g_ready = true;
    k_mutex_unlock(&g_lock);
}
