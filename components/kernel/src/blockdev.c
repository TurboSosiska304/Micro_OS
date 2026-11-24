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

#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/port.h"

#include <string.h>

/* The block layer's own mutex: the table is a critical section, the cache is
 * held across a read of the medium and has to sleep. */
static k_mutex_t g_blockdev_lock = K_MUTEX_INITIALIZER;

/*
 * The cache of one device. The boxes are permanent, because the view of a
 * device points at one of them and the table moves its entries when a
 * device leaves; a box is reused by the next device that asks for a cache.
 *
 * The number of the boxes is the one the device asked for and not a
 * constant: a flash chip of the board wants sixteen of them, a disk behind a
 * bus master wants the run its commands move (kernel/blockdev.h). The
 * bookkeeping is allocated with the memory of the copy and not carried in
 * the structure, so a device that asks for sixteen pays for sixteen.
 *
 * A cache of sixteen boxes can be searched by walking it: sixteen steps are
 * nothing next to a command of a flash chip. A cache of half a megabyte
 * cannot - five hundred steps for every sector a filesystem touches is more
 * work than the sector - so the boxes are found through a table of open
 * addressing, and the box that leaves is the one the hand of the cache points
 * at. Neither the search nor the choice walks the boxes.
 */
struct k_blockdev_cache_box {
    k_u32_t sector;
    k_u8_t valid;
    k_u8_t dirty;
};

struct k_blockdev_cache {
    bool used;
    bool live;         /* the memory of the cache is allocated */
    bool nocache;      /* the memory was asked for and refused */
    bool write_back;   /* a write waits for the room or for a sync */
    k_size_t capacity; /* the boxes, rounded up to a power of two */
    const struct k_blockdev *owner;
    struct k_blockdev view;
    struct k_blockdev_ops ops;
    k_u8_t *memory;  /* `capacity` sectors of the copy */
    k_u8_t *staging; /* the same, for a run that leaves the cache in one command */
    struct k_blockdev_cache_box *boxes;
    k_u32_t *index; /* `slots` entries: the box plus one, zero is empty */
    k_u32_t slots;  /* four times the boxes, a power of two */
    k_u32_t shift;  /* 32 minus the bits of `slots`: the hash of a sector */
    k_u32_t hand;   /* the hand of the cache: the next box to leave */

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

/*
 * The index over the boxes: open addressing with linear probing, four slots
 * for every box.
 *
 * The slot is not the low bits of the sector. A file is a run of sectors and
 * its blocks are a megabyte apart twice as often as anything else, so the low
 * bits of the number put the run of a file, the bitmap of its group and the
 * table of its inodes in one cluster of probes - and a cluster that holds a
 * run of five hundred sectors is a walk of five hundred for every sector
 * touched, which is the cost this index exists to remove. The golden ratio
 * spreads a run over the whole table instead: two neighbours land far apart
 * and two sectors a megabyte apart land wherever they land.
 *
 * The table is four times the boxes, so it is never more than a quarter full
 * and a probe is one or two steps.
 */
static k_u32_t cache_slot(const struct k_blockdev_cache *cache, k_u32_t sector)
{
    return (k_u32_t)((sector * 2654435761u) >> cache->shift);
}

static int cache_lookup(const struct k_blockdev_cache *cache, k_u32_t sector)
{
    k_u32_t slot = cache_slot(cache, sector);

    while (cache->index[slot] != 0u) {
        k_u32_t box = cache->index[slot] - 1u;

        if (cache->boxes[box].sector == sector) {
            return (int)box;
        }

        slot = (slot + 1u) & (cache->slots - 1u);
    }

    return -1;
}

static void cache_index_add(struct k_blockdev_cache *cache, k_size_t box)
{
    k_u32_t slot = cache_slot(cache, cache->boxes[box].sector);

    while (cache->index[slot] != 0u) {
        slot = (slot + 1u) & (cache->slots - 1u);
    }

    cache->index[slot] = (k_u32_t)box + 1u;
}

/*
 * Take a box out of the index. A hole in the middle of a cluster of probing
 * would hide every entry behind it, so the entries of the cluster are shifted
 * back into the hole one at a time - each one only when its own home slot is
 * not inside the interval it would jump over, which is what keeps every entry
 * reachable from its home.
 */
static void cache_index_remove(struct k_blockdev_cache *cache, k_size_t box)
{
    k_u32_t mask = cache->slots - 1u;
    k_u32_t hole = cache_slot(cache, cache->boxes[box].sector);
    k_u32_t probe = hole;

    while (cache->index[hole] != 0u && cache->index[hole] - 1u != box) {
        hole = (hole + 1u) & mask;
    }

    if (cache->index[hole] == 0u) {
        return; /* a box that was never indexed */
    }

    /* The slot of the box is the hole, and it is empty from this moment: an
     * entry that is not cleared stays in the table for ever, pointing at a
     * box that belongs to another sector by then, and the probes of the
     * cluster behind it walk a table that is all stale entries. */
    cache->index[hole] = 0u;

    for (;;) {
        k_u32_t home;

        probe = (probe + 1u) & mask;

        if (cache->index[probe] == 0u) {
            break;
        }

        home = cache_slot(cache, cache->boxes[cache->index[probe] - 1u].sector);

        /* The entry may take the hole when its home is not between the hole
         * and itself: an entry that jumped over its own home could never be
         * found again. */
        if ((hole <= probe) ? (home <= hole || home > probe) : (home <= hole && home > probe)) {
            cache->index[hole] = cache->index[probe];
            cache->index[probe] = 0u;
            hole = probe;
        }
    }
}

/*
 * The box that leaves, and the hand of the cache moves on.
 *
 * The box is the one the hand stood on longest, which is the oldest arrival
 * and not the least recently used one. Looking for the least recently used
 * box is a walk of the whole cache, and a cache that gives a second chance
 * to the boxes touched since its last turn walks the whole cache as well when
 * every box was touched - which is exactly the state of a file being written,
 * one box after another. A walk of five hundred per sector is the cost the
 * index was built to remove, so the order is the order of the arrivals: a run
 * of a file then leaves the cache in the order it went in, which is the order
 * the medium wants, and the metadata of a volume - touched again within a
 * turn of half a megabyte - is read again for what a command of a disk costs.
 */
static int cache_victim(struct k_blockdev_cache *cache)
{
    k_u32_t box = cache->hand;

    cache->hand = (box + 1u) & (k_u32_t)(cache->capacity - 1u);

    return (int)box;
}

static void cache_invalidate(struct k_blockdev_cache *cache)
{
    k_size_t i;

    for (i = 0; i < cache->capacity; i++) {
        cache->boxes[i].valid = false;
        cache->boxes[i].dirty = false;
    }

    if (cache->index != NULL) {
        memset(cache->index, 0, cache->slots * sizeof(cache->index[0]));
    }

    cache->hand = 0;
}

static int cache_flush_run(struct k_blockdev_cache *cache, int box);

/* The box that a sector is about to fill, with the room for it made when the
 * cache is full: the box is put in the index here, and its sector is the
 * sector of the caller from this moment. */
static int cache_place(struct k_blockdev_cache *cache, k_u32_t sector, int *box)
{
    int found = cache_lookup(cache, sector);
    int victim;

    if (found >= 0) {
        *box = found;
        return 0;
    }

    victim = cache_victim(cache);

    if (cache->boxes[victim].valid) {
        if (cache->boxes[victim].dirty) {
            if (cache_flush_run(cache, victim) != 0) {
                return -1;
            }
        }

        cache_index_remove(cache, (k_size_t)victim);
        cache->boxes[victim].valid = false;
        cache->boxes[victim].dirty = false;
    }

    cache->boxes[victim].sector = sector;
    cache->boxes[victim].valid = true;
    cache->boxes[victim].dirty = false;

    cache_index_add(cache, (k_size_t)victim);

    *box = victim;

    return 0;
}

/*
 * Give the medium the run of sectors from `first` to `last`: one command of
 * the device when the machine has a buffer to gather them in, and one sector
 * at a time when it has not. The run leaves the staging buffer when there is
 * one, so the sectors are copied out of their boxes in the order of the
 * medium and the device is handed one buffer.
 */
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
            cache->boxes[box].dirty = false;
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
    k_u32_t first = cache->boxes[box].sector;
    k_u32_t last = first;
    k_size_t i;

    for (;;) {
        int next = cache_lookup(cache, last + 1u);

        if (next < 0 || !cache->boxes[next].dirty) {
            break;
        }

        last = cache->boxes[next].sector;
    }

    for (;;) {
        int prev;

        if (first == 0) {
            break;
        }

        prev = cache_lookup(cache, first - 1u);

        if (prev < 0 || !cache->boxes[prev].dirty) {
            break;
        }

        first = cache->boxes[prev].sector;
    }

    if (cache_store(cache, first, last) != 0) {
        for (i = 0; i < cache->capacity; i++) {
            if (cache->boxes[i].valid && cache->boxes[i].sector >= first &&
                cache->boxes[i].sector <= last) {
                cache->boxes[i].valid = false;
                cache->boxes[i].dirty = false;
            }
        }

        return -1;
    }

    return 0;
}

static int cache_flush_all(struct k_blockdev_cache *cache)
{
    k_size_t i;

    for (i = 0; i < cache->capacity; i++) {
        if (cache->boxes[i].valid && cache->boxes[i].dirty) {
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

    if (cache->owner == NULL || cache->owner->sector_size == 0 || cache->capacity == 0) {
        return false;
    }

    bytes = cache->capacity * cache->owner->sector_size;

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

    /* The bookkeeping of the boxes and of the index is small and it is not
     * optional: without it there is no cache to speak of, and the memory of
     * the copy goes back to the manager. */
    cache->boxes = kmm_alloc(cache->capacity * sizeof(*cache->boxes), KMM_PRIORITY_KERNEL);
    cache->index = kmm_alloc(cache->slots * sizeof(cache->index[0]), KMM_PRIORITY_KERNEL);

    if (cache->boxes == NULL || cache->index == NULL) {
        if (cache->boxes != NULL) {
            kmm_free(cache->boxes);
            cache->boxes = NULL;
        }

        if (cache->index != NULL) {
            kmm_free(cache->index);
            cache->index = NULL;
        }

        kmm_free(cache->memory);
        cache->memory = NULL;

        if (cache->staging != NULL) {
            kmm_free(cache->staging);
            cache->staging = NULL;
        }

        cache->nocache = true;
        return false;
    }

    cache_invalidate(cache);
    cache->live = true;

    return true;
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

    if (count >= cache->capacity) {
        /* A run of the size of the cache is the whole cache: the boxes are
         * taken in order, because the box of the first sector would be the
         * one the hand reaches first and the last sector would evict it. */
        cache_invalidate(cache);

        for (i = 0; i < cache->capacity; i++) {
            memcpy(cache_box(cache, i), cache->staging + i * cache->owner->sector_size,
                   cache->owner->sector_size);
            cache->boxes[i].valid = true;
            cache->boxes[i].dirty = false;
            cache->boxes[i].sector = first + (k_u32_t)i;
            cache_index_add(cache, i);
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
        cache->boxes[box].valid = true;
        cache->boxes[box].dirty = false;
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
    if (count > cache->capacity / 4) {
        return dev->ops->read(dev->ctx, sector, buf, count);
    }

    if (k_mutex_lock(&g_blockdev_lock) != 0) {
        return dev->ops->read(dev->ctx, sector, buf, count);
    }

    if (!cache_ready(cache)) {
        k_mutex_unlock(&g_blockdev_lock);
        return dev->ops->read(dev->ctx, sector, buf, count);
    }

    /*
     * A call that starts where the last one ended continues the stream of the
     * caller: the medium is asked for the read ahead of the device and not for
     * the count of the call. That is the whole meaning of the two expectations,
     * and they are read once, here, because a call that does not continue is a
     * call of its own: the sectors it misses are gathered into one command of
     * the medium the size of the call and no bigger.
     *
     * Before ADR 0042 the expectation was armed from inside the call
     * (`jump_next = want + 1` after the first sector of a miss), so a run of
     * more than one sector that began on a miss read the read ahead of the
     * whole device for the four sectors of a record - an ext2 volume of 4 KiB
     * blocks over 1 KiB sectors read 36 MiB of a 16 MiB file.
     */
    bool stream = (cache->have_stream && sector == cache->stream_next) ||
                  (cache->have_jump && sector == cache->jump_next);
    bool prefetched = false;

    for (i = 0; i < count; i++) {
        k_u32_t want = sector + (k_u32_t)i;
        int box = cache_lookup(cache, want);

        if (box < 0) {
            k_u32_t rest = (k_u32_t)(count - i);
            k_u32_t ahead = rest;

            if (stream) {
                k_u32_t run =
                    dev->read_ahead != 0u ? dev->read_ahead : (k_u32_t)K_BLOCKDEV_READ_AHEAD;

                /* The staging buffer of the cache is the largest run it can
                 * read: a device that asks for more than the boxes it gave
                 * the cache is asking for a buffer that is not there. */
                if (run > cache->capacity) {
                    run = (k_u32_t)cache->capacity;
                }

                if (run > ahead) {
                    ahead = run;
                }
            }

            /* The end of the device is the end of the run: the tail of a
             * volume is read for what is there and not for what is not. */
            if (want < dev->sectors && ahead > (k_u32_t)(dev->sectors - want)) {
                ahead = (k_u32_t)(dev->sectors - want);
            }

            if (ahead > 1 && want < dev->sectors && cache->staging != NULL &&
                cache_flush_all(cache) == 0) {
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
                    stream = false; /* one command of the medium for one call */
                    prefetched = true;
                    box = cache_lookup(cache, want);
                }
            }

            if (box < 0) {
                if (cache_place(cache, want, &box) != 0) {
                    k_mutex_unlock(&g_blockdev_lock);
                    return -1;
                }

                if (dev->ops->read(dev->ctx, want, cache_box(cache, (k_size_t)box), 1) != 0) {
                    cache_index_remove(cache, (k_size_t)box);
                    cache->boxes[box].valid = false;
                    cache->boxes[box].dirty = false;
                    k_mutex_unlock(&g_blockdev_lock);
                    return -1;
                }

                cache->boxes[box].dirty = false;
            }
        }

        memcpy(dst + i * dev->sector_size, cache_box(cache, (k_size_t)box), dev->sector_size);
    }

    /*
     * The sector the next call is expected to ask for when it continues this
     * one: the one after the last sector of the call. A caller of one sector
     * at a time - MicroFS, a walk of the records of a directory - is what the
     * expectation was built for, and for it a call is one sector and this is
     * the next one. A prefetch has named its own end in `stream_next` already.
     */
    if (!prefetched) {
        cache->jump_next = sector + (k_u32_t)count;
        cache->have_jump = true;
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
                cache_index_remove(cache, (k_size_t)box);
                cache->boxes[box].valid = false;
                cache->boxes[box].dirty = false;
            }
        }

        k_mutex_unlock(&g_blockdev_lock);

        return rc;
    }

    /* Write-back. A run longer than the cache is written to the medium as it
     * is: the cache is a window of the working set, not a buffer of a file. */
    if (count > cache->capacity / 2) {
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
        cache->boxes[box].dirty = true;
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

/*
 * The door of the writes crosses the cache: the mount is above the cache and
 * the controller is below it, and the sector the cache holds has to be
 * written by a driver that was told to write. A cache with a write-back
 * policy is emptied before the door closes, so a sector it still holds is
 * not lost (the two disks of ADR 0041 are write-through and have none).
 */
static int cache_writes(void *ctx, bool open)
{
    struct k_blockdev_cache *cache = ctx;
    const struct k_blockdev *dev = cache->owner;

    if (!open && k_mutex_lock(&g_blockdev_lock) == 0) {
        if (cache->live) {
            (void)cache_flush_all(cache);
        }

        k_mutex_unlock(&g_blockdev_lock);
    }

    return dev->ops->writes(dev->ctx, open);
}

/* --- the table ------------------------------------------------------------ */

/*
 * The boxes of a device, rounded up to a power of two: the index is a mask of
 * the low bits of the sector and the clock is a mask of the low bits of the
 * hand, and both are a subtraction and not a division. A device that asks for
 * a hundred boxes gets a hundred and twenty-eight, which is the same cache
 * with a little more room.
 */
static k_size_t round_up_power_of_two(k_size_t value)
{
    k_size_t power = 1;

    while (power < value && power < K_BLOCKDEV_CACHE_MAX) {
        power <<= 1;
    }

    return power;
}

static struct k_blockdev_cache *cache_alloc(k_size_t capacity)
{
    k_size_t i;

    capacity = round_up_power_of_two(capacity);

    for (i = 0; i < K_BLOCKDEV_MAX; i++) {
        if (!g_caches[i].used) {
            k_u32_t bits = 0;
            k_u32_t slots = (k_u32_t)(capacity * 4u);

            while ((1u << bits) < slots) {
                bits++;
            }

            memset(&g_caches[i], 0, sizeof(g_caches[i]));
            g_caches[i].used = true;
            g_caches[i].capacity = capacity;
            g_caches[i].slots = slots;
            g_caches[i].shift = 32u - bits;
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

static int blockdev_add(const struct k_blockdev *dev, bool cached, bool write_back,
                        k_size_t capacity)
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

    if (cached && capacity > K_BLOCKDEV_CACHE_MAX) {
        capacity = K_BLOCKDEV_CACHE_MAX;
    }

    if (cached) {
        cache = cache_alloc(capacity);

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
            cache->ops.writes = dev->ops->writes != NULL ? cache_writes : NULL;
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
    return blockdev_add(dev, false, false, 0);
}

int k_blockdev_register_cached(const struct k_blockdev *dev, bool write_back)
{
    return blockdev_add(dev, true, write_back, K_BLOCKDEV_CACHE_SECTORS);
}

int k_blockdev_register_cached_sized(const struct k_blockdev *dev, bool write_back,
                                     k_size_t sectors)
{
    if (sectors == 0) {
        return -1;
    }

    return blockdev_add(dev, true, write_back, sectors);
}

int k_blockdev_unregister(const struct k_blockdev *dev)
{
    struct k_blockdev_cache *cache = NULL;
    void *memory = NULL;
    void *staging = NULL;
    void *boxes = NULL;
    void *index = NULL;
    k_size_t slot;
    int rc = -1;

    if (dev == NULL) {
        return -1;
    }

    k_port_enter_critical();

    if (entry_of(dev) != NULL) {
        slot = (k_size_t)(entry_of(dev) - g_entries);
        cache = g_entries[slot].cache;

        /* The table keeps its order: the devices after this one move down a
         * place, so "the n-th device" still means what it meant. */
        for (; slot + 1 < g_count; slot++) {
            g_entries[slot] = g_entries[slot + 1];
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
        boxes = cache->boxes;
        index = cache->index;
        memset(cache, 0, sizeof(*cache));
        k_mutex_unlock(&g_blockdev_lock);
    }

    if (memory != NULL) {
        kmm_free(memory);
    }

    if (staging != NULL) {
        kmm_free(staging);
    }

    if (boxes != NULL) {
        kmm_free(boxes);
    }

    if (index != NULL) {
        kmm_free(index);
    }

    return 0;
}

/*
 * What the cache of a device holds at this moment. The caller names the
 * device it knows - the view the table handed out, or the structure the
 * platform registered - and the answer is the same for both: the entry keeps
 * the pair, and a device without a cache says `present` false and no more.
 *
 * The read is not under the mutex of the cache: it is a report for a person
 * reading /proc and not a decision of the filesystem, and a count that is one
 * sector off is the honest price of not stopping the disk to ask.
 */
int k_blockdev_cache_stats(const struct k_blockdev *dev, struct k_blockdev_cache_stats *out)
{
    const struct k_blockdev_entry *entry;
    const struct k_blockdev_cache *cache;
    k_size_t i;

    if (dev == NULL || out == NULL) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    k_port_enter_critical();

    entry = entry_of(dev);

    if (entry == NULL) {
        k_port_exit_critical();
        return -1;
    }

    out->sectors = entry->owner->sectors;
    out->sector_size = entry->owner->sector_size;

    cache = entry->cache;

    if (cache != NULL) {
        out->present = true;
        out->live = cache->live;
        out->write_back = cache->write_back;
        out->capacity = (k_u32_t)cache->capacity;

        if (cache->live && cache->boxes != NULL) {
            for (i = 0; i < cache->capacity; i++) {
                if (!cache->boxes[i].valid) {
                    continue;
                }

                out->valid++;

                if (cache->boxes[i].dirty) {
                    out->dirty++;
                }
            }
        }
    }

    k_port_exit_critical();

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

int k_blockdev_sync_all(void)
{
    const struct k_blockdev *devs[K_BLOCKDEV_MAX];
    k_size_t count = 0;
    k_size_t i;
    int rc = 0;

    /*
     * The table is copied under the critical section and the devices are
     * asked outside of it: a sync of a disk is a command that waits for an
     * interrupt, and an interrupt off across it would hold the machine for
     * the time of a platter.
     */
    k_port_enter_critical();

    for (i = 0; i < g_count; i++) {
        devs[count++] = g_entries[i].dev;
    }

    k_port_exit_critical();

    for (i = 0; i < count; i++) {
        const struct k_blockdev *dev = devs[i];

        if (dev == NULL || dev->ops == NULL || dev->ops->sync == NULL) {
            continue;
        }

        if (dev->ops->sync(dev->ctx) != 0) {
            kerror("blockdev: %s did not answer the flush", dev->name);
            rc = KERR_IO;
        }
    }

    return rc;
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

/*
 * The door of the writes of a medium (ADR 0041 §2). The layers that stand
 * between a mount and a controller - the cache, a partition, a view - carry
 * the operation of the medium below them, so a device either has the whole
 * chain or none of it, and a mount asks the name it mounted.
 */
bool k_blockdev_gated(const struct k_blockdev *dev)
{
    return dev != NULL && dev->ops != NULL && dev->ops->writes != NULL;
}

int k_blockdev_writes(const struct k_blockdev *dev, bool open)
{
    if (dev == NULL || dev->ops == NULL) {
        return KERR_INVAL;
    }

    if (dev->ops->writes == NULL) {
        return KERR_NOTSUP;
    }

    return dev->ops->writes(dev->ctx, open);
}

/* --- the grain of a volume ------------------------------------------------ */

/*
 * A view is arithmetic over one call: the sector and the count of the view are
 * multiplied, and the device below takes the run. Nothing is copied and nothing
 * is buffered here - the cache of the medium, when it has one, sits below the
 * view and gathers the run into the commands of the device (ADR 0032) - so the
 * only thing the view changes is how many times the layer above asks.
 */
static bool view_span(const struct k_blockdev_view *view, k_u32_t sector, k_size_t count,
                      k_u64_t *first, k_size_t *sectors)
{
    k_u64_t start = (k_u64_t)sector * view->factor;
    k_u64_t run = (k_u64_t)count * view->factor;

    if (first == NULL || sectors == NULL) {
        return false;
    }

    if (start > (k_u64_t)view->medium->sectors || run > (k_u64_t)view->medium->sectors - start) {
        return false;
    }

    *first = start;
    *sectors = (k_size_t)run;

    return true;
}

static int view_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct k_blockdev_view *view = ctx;
    k_u64_t first = 0;
    k_size_t sectors = 0;

    if (buf == NULL || !view_span(view, sector, count, &first, &sectors)) {
        return -1;
    }

    if (sectors == 0) {
        return 0;
    }

    view->reads++;
    view->sectors_read += (k_u64_t)sectors;

    return view->medium->ops->read(view->medium->ctx, (k_u32_t)first, buf, sectors);
}

static int view_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct k_blockdev_view *view = ctx;
    k_u64_t first = 0;
    k_size_t sectors = 0;

    if (buf == NULL || !view_span(view, sector, count, &first, &sectors)) {
        return -1;
    }

    if (sectors == 0) {
        return 0;
    }

    view->writes++;
    view->sectors_written += (k_u64_t)sectors;

    return view->medium->ops->write(view->medium->ctx, (k_u32_t)first, buf, sectors);
}

static int view_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    struct k_blockdev_view *view = ctx;
    k_u64_t first = 0;
    k_size_t sectors = 0;

    if (!view_span(view, sector, count, &first, &sectors)) {
        return -1;
    }

    if (sectors == 0) {
        return 0;
    }

    return view->medium->ops->erase(view->medium->ctx, (k_u32_t)first, sectors);
}

static int view_sync(void *ctx)
{
    struct k_blockdev_view *view = ctx;

    return view->medium->ops->sync(view->medium->ctx);
}

/* The door of the writes is the medium's: a view is a way of reading and
 * writing the same bytes, not a medium with a rule of its own. */
static int view_writes(void *ctx, bool open)
{
    struct k_blockdev_view *view = ctx;

    return view->medium->ops->writes(view->medium->ctx, open);
}

int k_blockdev_view_open(struct k_blockdev_view *view, const struct k_blockdev *medium,
                         k_u32_t sector_size)
{
    if (view == NULL || medium == NULL || medium->ops == NULL || medium->ops->read == NULL ||
        medium->ops->write == NULL) {
        return -1;
    }

    /*
     * The grain of a view is a whole number of the medium's sectors and more
     * than one of them: a view of the grain of the medium is the medium, and a
     * sector of the medium is not a fraction of anything.
     */
    if (medium->sector_size == 0 || sector_size <= medium->sector_size ||
        (sector_size % medium->sector_size) != 0) {
        return -1;
    }

    memset(view, 0, sizeof(*view));

    view->factor = sector_size / medium->sector_size;

    if (medium->sectors < view->factor) {
        return -1; /* the medium is shorter than one sector of the view */
    }

    view->medium = medium;
    view->ops.read = view_read;
    view->ops.write = view_write;
    view->ops.erase = (medium->ops->erase != NULL) ? view_erase : NULL;
    view->ops.sync = (medium->ops->sync != NULL) ? view_sync : NULL;
    view->ops.writes = (medium->ops->writes != NULL) ? view_writes : NULL;

    view->dev.name = medium->name;
    view->dev.sector_size = sector_size;
    view->dev.sectors = medium->sectors / view->factor;
    view->dev.ops = &view->ops;
    view->dev.ctx = view;
    /* The read ahead belongs to the cache of the medium: a view has no cache
     * of its own, and nothing above it reads this number. */
    view->dev.read_ahead = 0;

    return 0;
}

void k_blockdev_view_close(struct k_blockdev_view *view)
{
    if (view == NULL) {
        return;
    }

    memset(view, 0, sizeof(*view));
}
