/*
 * Micro OS — the block devices of the system.
 *
 * A table of pointers, no more: the devices live in the platform, the
 * kernel only knows where they are. The table is guarded by the critical
 * section because the shell may mount a device while another task is
 * enumerating them - the sections are three instructions long.
 *
 * One thing of the kernel sits between the table and the device: the cache
 * of a device that asked for one (k_blockdev_register_cached). The
 * filesystems of ADR 0009 all work a sector at a time, and the metadata of a
 * volume - the bitmap of the free blocks, the table of the inodes, the
 * descriptor of a group - is read and written again for every block the
 * volume allocates.
 * Without the cache every one of those touches is a command of the medium:
 * a platter pays for the seek, and a card or a flash chip pays for the
 * transaction of the driver.
 *
 * The cache is the last sixteen sectors of the device, and it is read
 * through. What it does with a write is the choice of the device:
 *
 *   write-through (the default) - the medium takes the sector, then the
 *   copy of it moves. A crash loses nothing that a call of the filesystem
 *   returned, and a device that holds nothing back needs no sync of its own.
 *
 *   write-back (the second argument of k_blockdev_register_cached) - the
 *   sector goes into the cache and reaches the medium when the cache needs
 *   the room, when the filesystem asks for a sync, or when the device is
 *   unmounted. A run of sectors that
 *   leaves the cache together is one command of the medium, which is what
 *   makes a filesystem that writes a sector at a time fast: sixteen sectors
 *   of a file and one write of the bitmap behind them cost two commands and
 *   not seventeen. The price is the one every cache of a disk has - a
 *   machine that stops without a sync loses the sectors in flight, sixteen
 *   of them at the most.
 *
 * The cache lives in a view of the device: the structure the platform
 * registered is not the pointer the table hands out, because a device knows
 * nothing about the cache above it. The view has the name, the size and the
 * context of the device and its own pair of operations, and the memory of
 * the cache hangs on it.
 */
#include "kernel/blockdev.h"

#include "kernel/kmm.h"
#include "kernel/lock.h"
#include "kernel/port.h"

#include <string.h>

/* The block layer's own mutex: the table is a critical section, the cache is
 * held across a read of the medium and has to sleep. */
static k_mutex_t g_blockdev_lock = K_MUTEX_INITIALIZER;

/*
 * The cache of one device. The boxes are permanent, because the view of a
 * device points at one of them and the table moves its entries when a
 * device leaves; a box is reused by the next device that asks for a cache.
 */
struct k_blockdev_cache {
    bool used;
    bool live;       /* the memory of the cache is allocated */
    bool nocache;    /* the memory was asked for and refused */
    bool write_back; /* a write waits for the room or for a sync */
    const struct k_blockdev *owner;
    struct k_blockdev view;
    struct k_blockdev_ops ops;
    k_u8_t *memory;  /* K_BLOCKDEV_CACHE_SECTORS sectors of the copy */
    k_u8_t *staging; /* the same, for a run that leaves the cache in one command */
    k_u32_t sector[K_BLOCKDEV_CACHE_SECTORS];
    k_u32_t age[K_BLOCKDEV_CACHE_SECTORS];
    k_u8_t valid[K_BLOCKDEV_CACHE_SECTORS];
    k_u8_t dirty[K_BLOCKDEV_CACHE_SECTORS];
    k_u32_t clock;

    /* The two sectors a stream of reads is expected to ask for next: the one
     * that continues a run of the medium, and the one that continues the
     * last read out of the blue - a file that starts in the middle of the
     * volume, a directory walk that jumps to another group. A read of either
     * is a read ahead; the second is what starts a new stream when the first
     * one is interrupted by a read of the metadata. */
    k_u32_t stream_next;
    k_u32_t jump_next;
    bool have_stream;
    bool have_jump;
};

/* One entry of the table: what the platform registered and what is handed
 * out. The two are the same pointer for a device without a cache. */
struct k_blockdev_entry {
    const struct k_blockdev *dev;
    const struct k_blockdev *owner;
    struct k_blockdev_cache *cache;
};

static struct k_blockdev_entry g_entries[K_BLOCKDEV_MAX];
static struct k_blockdev_cache g_caches[K_BLOCKDEV_MAX];
static k_size_t g_count;

/* --- the boxes of the cache ----------------------------------------------- */

static k_u8_t *cache_box(const struct k_blockdev_cache *cache, k_size_t box)
{
    return cache->memory + box * cache->owner->sector_size;
}

static int cache_lookup(const struct k_blockdev_cache *cache, k_u32_t sector)
{
    k_size_t i;

    for (i = 0; i < K_BLOCKDEV_CACHE_SECTORS; i++) {
        if (cache->valid[i] && cache->sector[i] == sector) {
            return (int)i;
        }
    }

    return -1;
}

/* The box of the next sector: the empty one, or the oldest. */
static int cache_victim(const struct k_blockdev_cache *cache)
{
    int oldest = 0;
    k_size_t i;

    for (i = 0; i < K_BLOCKDEV_CACHE_SECTORS; i++) {
        if (!cache->valid[i]) {
            return (int)i;
        }

        if (cache->age[i] < cache->age[oldest]) {
            oldest = (int)i;
        }
    }

    return oldest;
}

static void cache_invalidate(struct k_blockdev_cache *cache)
{
    memset(cache->valid, 0, sizeof(cache->valid));
    memset(cache->dirty, 0, sizeof(cache->dirty));
}

static int cache_store(struct k_blockdev_cache *cache, k_u32_t first, k_u32_t last)
{
    const struct k_blockdev *dev = cache->owner;
    k_size_t run = (k_size_t)(last - first) + 1u;
    k_size_t i;

    if (cache->staging != NULL) {
        for (i = 0; i < run; i++) {
            int box = cache_lookup(cache, first + (k_u32_t)i);

            if (box < 0) {
                return -1;
            }

            memcpy(cache->staging + i * dev->sector_size, cache_box(cache, (k_size_t)box),
                   dev->sector_size);
        }

        if (dev->ops->write(dev->ctx, first, cache->staging, run) != 0) {
            return -1;
        }
    } else {
        /* No room for the staging: the sectors go one at a time. It is the
         * slow way and the same bytes. */
        for (i = 0; i < run; i++) {
            int box = cache_lookup(cache, first + (k_u32_t)i);

            if (box < 0 || dev->ops->write(dev->ctx, first + (k_u32_t)i,
                                           cache_box(cache, (k_size_t)box), 1) != 0) {
                return -1;
            }
        }
    }

    for (i = 0; i < run; i++) {
        int box = cache_lookup(cache, first + (k_u32_t)i);

        if (box >= 0) {
            cache->dirty[box] = false;
        }
    }

    return 0;
}

/*
 * Give the medium the run of dirty sectors that holds `box`: the box and the
 * dirty sectors next to it, forwards and backwards, as one command. The
 * sectors of a file are written one after another, so the run is the sixteen
 * sectors of the cache and not one.
 *
 * A write that fails drops the run: the sectors of a broken medium are lost
 * either way, and a copy that stays in the cache would be written again over
 * a sector somebody else may own by then.
 */
static int cache_flush_run(struct k_blockdev_cache *cache, int box)
{
    k_u32_t first = cache->sector[box];
    k_u32_t last = first;
    k_size_t i;

    for (;;) {
        int next = cache_lookup(cache, last + 1u);

        if (next < 0 || !cache->dirty[next]) {
            break;
        }

        last = cache->sector[next];
    }

    for (;;) {
        int prev;

        if (first == 0) {
            break;
        }

        prev = cache_lookup(cache, first - 1u);

        if (prev < 0 || !cache->dirty[prev]) {
            break;
        }

        first = cache->sector[prev];
    }

    if (cache_store(cache, first, last) != 0) {
        for (i = 0; i < K_BLOCKDEV_CACHE_SECTORS; i++) {
            if (cache->valid[i] && cache->sector[i] >= first && cache->sector[i] <= last) {
                cache->valid[i] = false;
                cache->dirty[i] = false;
            }
        }

        return -1;
    }

    return 0;
}

static int cache_flush_all(struct k_blockdev_cache *cache)
{
    k_size_t i;

    for (i = 0; i < K_BLOCKDEV_CACHE_SECTORS; i++) {
        if (cache->valid[i] && cache->dirty[i]) {
            if (cache_flush_run(cache, (int)i) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

/*
 * The memory of the cache, the first time a device with a cache is read.
 *
 * It cannot be allocated at the registration: the platforms register their
 * devices before the kernel boots, and the memory manager of the kernel is
 * born during the boot. The first read comes from a filesystem, which lives
 * after the boot, and that is soon enough. A machine that cannot give the
 * cache its memory (the internal RAM of the board is scarce) gets no cache
 * and not a failed read: the memory is the comfort of the boot, never a
 * condition of it.
 *
 * The priority is the one of the user, which the memory manager of a board
 * with PSRAM reads as "the PSRAM": the cache is a buffer of a disk and not
 * the kernel's own state, and the internal RAM of the board is for the
 * kernel.
 */
static bool cache_ready(struct k_blockdev_cache *cache)
{
    k_size_t bytes;

    if (cache->live) {
        return true;
    }

    if (cache->nocache) {
        return false;
    }

    if (cache->owner == NULL || cache->owner->sector_size == 0) {
        return false;
    }

    bytes = (k_size_t)K_BLOCKDEV_CACHE_SECTORS * cache->owner->sector_size;

    cache->memory = kmm_alloc(bytes, KMM_PRIORITY_USER);

    if (cache->memory == NULL) {
        /* A second read would fail the same way: the device is asked
         * directly from now on, and the cache is a matter of comfort. */
        cache->nocache = true;
        return false;
    }

    /* The staging is the fast way out of the cache and not the only one: a
     * machine that cannot give it the second buffer writes the sectors of a
     * run one at a time. */
    cache->staging = kmm_alloc(bytes, KMM_PRIORITY_USER);

    cache_invalidate(cache);
    cache->live = true;

    return true;
}

/* The box of a sector that is about to be filled, with the room for it made
 * when the cache is full. */
static int cache_place(struct k_blockdev_cache *cache, k_u32_t sector, int *box)
{
    int found = cache_lookup(cache, sector);
    int victim;

    if (found >= 0) {
        *box = found;
        return 0;
    }

    victim = cache_victim(cache);

    if (cache->valid[victim] && cache->dirty[victim]) {
        if (cache_flush_run(cache, victim) != 0) {
            return -1;
        }
    }

    cache->valid[victim] = false;
    cache->dirty[victim] = false;
    *box = victim;

    return 0;
}

/*
 * Fill the cache with a run of sectors that were just read into the staging
 * buffer: the read ahead of a file that is being read from its beginning to
 * its end.
 *
 * The caller has written every dirty box of the cache to the medium before
 * it read the run into the staging buffer: the staging is one buffer for
 * both directions, and a flush in the middle of this fill would overwrite
 * the run that is being copied out of it. Nothing here writes to the
 * medium, and every box the run lands in is free or clean.
 */
static int cache_fill_run(struct k_blockdev_cache *cache, k_u32_t first, k_size_t count)
{
    k_size_t i;

    if (count >= K_BLOCKDEV_CACHE_SECTORS) {
        /* A run of the size of the cache is the whole cache: the boxes are
         * taken in order, because the box of the first sector would be the
         * oldest of the run and the last sector would evict it. */
        for (i = 0; i < K_BLOCKDEV_CACHE_SECTORS; i++) {
            memcpy(cache_box(cache, i), cache->staging + i * cache->owner->sector_size,
                   cache->owner->sector_size);
            cache->valid[i] = true;
            cache->dirty[i] = false;
            cache->sector[i] = first + (k_u32_t)i;
            cache->age[i] = ++cache->clock;
        }

        return 0;
    }

    for (i = 0; i < count; i++) {
        k_u32_t want = first + (k_u32_t)i;
        int box = cache_lookup(cache, want);

        if (box < 0) {
            if (cache_place(cache, want, &box) != 0) {
                return -1;
            }
        }

        memcpy(cache_box(cache, (k_size_t)box), cache->staging + i * cache->owner->sector_size,
               cache->owner->sector_size);
        cache->valid[box] = true;
        cache->dirty[box] = false;
        cache->sector[box] = want;
        cache->age[box] = ++cache->clock;
    }

    return 0;
}

/* --- the operations of a cached device ------------------------------------ */

static int cache_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct k_blockdev_cache *cache = ctx;
    const struct k_blockdev *dev = cache->owner;
    k_u8_t *dst = buf;
    k_size_t i;

    if (buf == NULL) {
        return -1;
    }

    /* A run longer than the cache is a stream and not a working set: the
     * copy would evict itself before it is looked at again. */
    if (count > K_BLOCKDEV_CACHE_SECTORS / 4) {
        return dev->ops->read(dev->ctx, sector, buf, count);
    }

    if (k_mutex_lock(&g_blockdev_lock) != 0) {
        return dev->ops->read(dev->ctx, sector, buf, count);
    }

    if (!cache_ready(cache)) {
        k_mutex_unlock(&g_blockdev_lock);
        return dev->ops->read(dev->ctx, sector, buf, count);
    }

    for (i = 0; i < count; i++) {
        k_u32_t want = sector + (k_u32_t)i;
        int box = cache_lookup(cache, want);

        if (box < 0) {
            /*
             * A read that continues a run is a read ahead: one command of the
             * medium fills the cache with the sectors the next calls of the
             * filesystem will ask for. The run is interrupted all the time by
             * the metadata of the volume - the table of the pointers of the
             * file, the bitmap of the blocks - which is read between two
             * sectors of the file; the second expectation is what lets the
             * run continue after such a read, and what starts a new run when
             * a file begins somewhere else.
             */
            k_u32_t ahead = 1;

            if ((cache->have_stream && want == cache->stream_next) ||
                (cache->have_jump && want == cache->jump_next)) {
                if (want + K_BLOCKDEV_READ_AHEAD <= dev->sectors) {
                    ahead = K_BLOCKDEV_READ_AHEAD;
                }
            }

            if (ahead > 1 && cache->staging != NULL && cache_flush_all(cache) == 0) {
                /* The staging is the buffer of the run: what the cache holds
                 * back goes to the medium first, so that nothing writes to
                 * the staging while the run is copied out of it. A cache
                 * that cannot empty itself keeps its sectors and reads one
                 * at a time. */
                if (dev->ops->read(dev->ctx, want, cache->staging, ahead) == 0) {
                    if (cache_fill_run(cache, want, ahead) != 0) {
                        k_mutex_unlock(&g_blockdev_lock);
                        return -1;
                    }

                    cache->stream_next = want + ahead;
                    cache->have_stream = true;
                    cache->have_jump = false;
                    box = cache_lookup(cache, want);
                }
            }

            if (box < 0) {
                if (cache_place(cache, want, &box) != 0) {
                    k_mutex_unlock(&g_blockdev_lock);
                    return -1;
                }

                if (dev->ops->read(dev->ctx, want, cache_box(cache, (k_size_t)box), 1) != 0) {
                    cache->valid[box] = false;
                    cache->dirty[box] = false;
                    k_mutex_unlock(&g_blockdev_lock);
                    return -1;
                }

                cache->valid[box] = true;
                cache->dirty[box] = false;
                cache->sector[box] = want;

                /* A read out of the order of the stream does not end it: the
                 * metadata of the volume sits between two of its sectors.
                 * The next sector after this one is the seed of another. */
                cache->jump_next = want + 1u;
                cache->have_jump = true;
            }
        }

        cache->age[box] = ++cache->clock;
        memcpy(dst + i * dev->sector_size, cache_box(cache, (k_size_t)box), dev->sector_size);
    }

    k_mutex_unlock(&g_blockdev_lock);

    return 0;
}

static int cache_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct k_blockdev_cache *cache = ctx;
    const struct k_blockdev *dev = cache->owner;
    const k_u8_t *src = buf;
    int rc;
    k_size_t i;

    if (buf == NULL) {
        return -1;
    }

    /* The mutex is taken before the medium is asked, and held until the copy
     * follows the write: a reader of the same sector on another core finds
     * the cache in one of the two states and never in neither. */
    if (k_mutex_lock(&g_blockdev_lock) != 0) {
        return dev->ops->write(dev->ctx, sector, buf, count);
    }

    if (!cache_ready(cache)) {
        k_mutex_unlock(&g_blockdev_lock);
        return dev->ops->write(dev->ctx, sector, buf, count);
    }

    if (!cache->write_back) {
        /* Write-through: the medium takes the data before the copy of it
         * moves. A write that fails leaves nothing in the cache that the
         * medium does not have - the boxes of the run are dropped instead. */
        rc = dev->ops->write(dev->ctx, sector, buf, count);

        for (i = 0; i < count; i++) {
            int box = cache_lookup(cache, sector + (k_u32_t)i);

            if (box < 0) {
                continue;
            }

            if (rc == 0) {
                memcpy(cache_box(cache, (k_size_t)box), src + i * dev->sector_size,
                       dev->sector_size);
            } else {
                cache->valid[box] = false;
                cache->dirty[box] = false;
            }
        }

        k_mutex_unlock(&g_blockdev_lock);

        return rc;
    }

    /* Write-back. A run longer than the cache is written to the medium as it
     * is: the cache is a window of the working set, not a buffer of a file. */
    if (count > K_BLOCKDEV_CACHE_SECTORS / 2) {
        (void)cache_flush_all(cache);
        cache_invalidate(cache);
        rc = dev->ops->write(dev->ctx, sector, buf, count);
        cache_invalidate(cache);

        k_mutex_unlock(&g_blockdev_lock);

        return rc;
    }

    for (i = 0; i < count; i++) {
        k_u32_t want = sector + (k_u32_t)i;
        int box;

        if (cache_place(cache, want, &box) != 0) {
            k_mutex_unlock(&g_blockdev_lock);
            return -1;
        }

        memcpy(cache_box(cache, (k_size_t)box), src + i * dev->sector_size, dev->sector_size);
        cache->valid[box] = true;
        cache->dirty[box] = true;
        cache->sector[box] = want;
        cache->age[box] = ++cache->clock;
    }

    k_mutex_unlock(&g_blockdev_lock);

    return 0;
}

static int cache_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    struct k_blockdev_cache *cache = ctx;
    const struct k_blockdev *dev = cache->owner;
    int rc;

    if (k_mutex_lock(&g_blockdev_lock) != 0) {
        return dev->ops->erase(dev->ctx, sector, count);
    }

    if (cache->live) {
        (void)cache_flush_all(cache);
    }

    rc = dev->ops->erase(dev->ctx, sector, count);

    if (cache->live) {
        cache_invalidate(cache);
    }

    k_mutex_unlock(&g_blockdev_lock);

    return rc;
}

static int cache_sync(void *ctx)
{
    struct k_blockdev_cache *cache = ctx;
    const struct k_blockdev *dev = cache->owner;
    int rc = 0;

    if (k_mutex_lock(&g_blockdev_lock) == 0) {
        if (cache->live && cache_flush_all(cache) != 0) {
            rc = -1;
        }

        k_mutex_unlock(&g_blockdev_lock);
    }

    if (dev->ops->sync != NULL && dev->ops->sync(dev->ctx) != 0) {
        rc = -1;
    }

    return rc;
}

/* --- the table ------------------------------------------------------------ */

static struct k_blockdev_cache *cache_alloc(void)
{
    k_size_t i;

    for (i = 0; i < K_BLOCKDEV_MAX; i++) {
        if (!g_caches[i].used) {
            memset(&g_caches[i], 0, sizeof(g_caches[i]));
            g_caches[i].used = true;
            return &g_caches[i];
        }
    }

    return NULL;
}

static struct k_blockdev_entry *entry_of(const struct k_blockdev *dev)
{
    k_size_t i;

    for (i = 0; i < g_count; i++) {
        if (g_entries[i].dev == dev || g_entries[i].owner == dev) {
            return &g_entries[i];
        }
    }

    return NULL;
}

static int blockdev_add(const struct k_blockdev *dev, bool cached, bool write_back)
{
    struct k_blockdev_entry *entry;
    struct k_blockdev_cache *cache;

    if (dev == NULL || dev->ops == NULL || dev->ops->read == NULL || dev->ops->write == NULL) {
        return -1;
    }

    if (dev->sector_size == 0 || dev->sectors == 0) {
        return -1;
    }

    k_port_enter_critical();

    if (g_count >= K_BLOCKDEV_MAX) {
        k_port_exit_critical();
        return -1;
    }

    entry = &g_entries[g_count];

    memset(entry, 0, sizeof(*entry));
    entry->dev = dev;
    entry->owner = dev;

    if (cached) {
        cache = cache_alloc();

        if (cache != NULL) {
            cache->owner = dev;
            cache->write_back = write_back;
            cache->view = *dev;
            cache->view.ctx = cache;
            cache->ops = *dev->ops;
            cache->ops.read = cache_read;
            cache->ops.write = cache_write;
            cache->ops.erase = dev->ops->erase != NULL ? cache_erase : NULL;
            cache->ops.sync = dev->ops->sync != NULL ? cache_sync : NULL;
            cache->view.ops = &cache->ops;

            entry->dev = &cache->view;
            entry->cache = cache;
        }
    }

    g_count++;

    k_port_exit_critical();

    return 0;
}

int k_blockdev_register(const struct k_blockdev *dev)
{
    return blockdev_add(dev, false, false);
}

int k_blockdev_register_cached(const struct k_blockdev *dev, bool write_back)
{
    return blockdev_add(dev, true, write_back);
}

int k_blockdev_unregister(const struct k_blockdev *dev)
{
    struct k_blockdev_cache *cache = NULL;
    void *memory = NULL;
    void *staging = NULL;
    k_size_t index;
    int rc = -1;

    if (dev == NULL) {
        return -1;
    }

    k_port_enter_critical();

    if (entry_of(dev) != NULL) {
        index = (k_size_t)(entry_of(dev) - g_entries);
        cache = g_entries[index].cache;

        /* The table keeps its order: the devices after this one move down a
         * place, so "the n-th device" still means what it meant. */
        for (; index + 1 < g_count; index++) {
            g_entries[index] = g_entries[index + 1];
        }

        memset(&g_entries[--g_count], 0, sizeof(g_entries[0]));
        rc = 0;
    }

    k_port_exit_critical();

    if (rc != 0) {
        return rc;
    }

    if (cache == NULL) {
        return 0;
    }

    /*
     * The caller promised that nobody uses the device any more, so what the
     * cache holds of it is written now: a device that leaves the table with
     * the last writes of a file in the cache would lose them.
     *
     * The free is outside the critical section - kmm_free takes a lock and
     * may not be called with the interrupts off - and outside the mutex of
     * the cache, which the box gives up first.
     */
    if (k_mutex_lock(&g_blockdev_lock) == 0) {
        if (cache->live) {
            (void)cache_flush_all(cache);
        }

        memory = cache->memory;
        staging = cache->staging;
        memset(cache, 0, sizeof(*cache));
        k_mutex_unlock(&g_blockdev_lock);
    }

    if (memory != NULL) {
        kmm_free(memory);
    }

    if (staging != NULL) {
        kmm_free(staging);
    }

    return 0;
}

k_size_t k_blockdev_count(void)
{
    k_size_t count;

    k_port_enter_critical();
    count = g_count;
    k_port_exit_critical();

    return count;
}

const struct k_blockdev *k_blockdev_at(k_size_t index)
{
    const struct k_blockdev *dev = NULL;

    k_port_enter_critical();

    if (index < g_count) {
        dev = g_entries[index].dev;
    }

    k_port_exit_critical();

    return dev;
}

const struct k_blockdev *k_blockdev_find(const char *name)
{
    const struct k_blockdev *found = NULL;
    k_size_t i;

    if (name == NULL) {
        return NULL;
    }

    k_port_enter_critical();

    for (i = 0; i < g_count; i++) {
        const char *other = g_entries[i].dev->name;
        k_size_t j = 0;

        while (other[j] != '\0' && name[j] != '\0' && other[j] == name[j]) {
            j++;
        }

        if (other[j] == '\0' && name[j] == '\0') {
            found = g_entries[i].dev;
            break;
        }
    }

    k_port_exit_critical();

    return found;
}
