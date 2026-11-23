/*
 * Micro OS — the cache of a block device.
 *
 * A device that asks for the cache of the block layer (k_blockdev::cached)
 * is read through a copy of its last sectors, and written through to the
 * medium. The test holds a device of the memory and counts the calls that
 * reach it: the count is the measure of the cache, and the bytes that come
 * back after a write are the measure of its correctness.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"

#define CACHE_SECTORS 64
#define CACHE_SECTOR_SIZE 32

static k_u8_t s_data[CACHE_SECTORS][CACHE_SECTOR_SIZE];
static k_size_t s_reads;
static k_size_t s_writes;
static k_size_t s_erases;
static k_size_t s_syncs;
static k_u32_t s_last_sector;
static k_size_t s_last_count;

static void fill_sector(k_u32_t sector)
{
    k_size_t i;

    for (i = 0; i < CACHE_SECTOR_SIZE; i++) {
        s_data[sector][i] = (k_u8_t)(sector * 7u + i);
    }
}

static int counting_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector + count > CACHE_SECTORS) {
        return -1;
    }

    s_reads++;
    memcpy(buf, s_data[sector], count * CACHE_SECTOR_SIZE);

    return 0;
}

static int counting_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector + count > CACHE_SECTORS) {
        return -1;
    }

    s_writes++;
    s_last_sector = sector;
    s_last_count = count;
    memcpy(s_data[sector], buf, count * CACHE_SECTOR_SIZE);

    return 0;
}

static int counting_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector + count > CACHE_SECTORS) {
        return -1;
    }

    s_erases++;
    memset(s_data[sector], 0, count * CACHE_SECTOR_SIZE);

    return 0;
}

static int counting_sync(void *ctx)
{
    K_UNUSED(ctx);

    s_syncs++;

    return 0;
}

static const struct k_blockdev_ops s_ops = {
    .read = counting_read,
    .write = counting_write,
    .erase = counting_erase,
    .sync = counting_sync,
};

/* A device of the memory without a cache: the table hands out the structure
 * the platform registered, as it always did. */
static const struct k_blockdev s_plain = {
    .name = "plain0",
    .sector_size = CACHE_SECTOR_SIZE,
    .sectors = CACHE_SECTORS,
    .ops = &s_ops,
    .ctx = NULL,
};

/* The same device with the cache of the block layer. */
static const struct k_blockdev s_cached = {
    .name = "cached0",
    .sector_size = CACHE_SECTOR_SIZE,
    .sectors = CACHE_SECTORS,
    .ops = &s_ops,
    .ctx = NULL,
};

/* And the one whose cache holds the writes back until it needs the room. */
static const struct k_blockdev s_writeback = {
    .name = "wb0",
    .sector_size = CACHE_SECTOR_SIZE,
    .sectors = CACHE_SECTORS,
    .ops = &s_ops,
    .ctx = NULL,
};

/* The one that asks for a cache of four boxes instead of the default. */
static const struct k_blockdev s_sized = {
    .name = "sized0",
    .sector_size = CACHE_SECTOR_SIZE,
    .sectors = CACHE_SECTORS,
    .ops = &s_ops,
    .ctx = NULL,
};

void test_blockdev_cache(void)
{
    const struct k_blockdev *view;
    k_u8_t buffer[CACHE_SECTOR_SIZE];
    k_u8_t expect[CACHE_SECTOR_SIZE];
    k_u8_t run[CACHE_SECTOR_SIZE * 8];
    k_size_t i;

    ktest_begin("blockdev-cache");

    for (i = 0; i < CACHE_SECTORS; i++) {
        fill_sector((k_u32_t)i);
    }

    /* --- a device without a cache keeps its identity --------------------- */

    KT_CHECK_INT_EQ(k_blockdev_register(&s_plain), 0);
    KT_CHECK(k_blockdev_find("plain0") == &s_plain);
    KT_CHECK_INT_EQ(k_blockdev_unregister(&s_plain), 0);
    KT_CHECK(k_blockdev_find("plain0") == NULL);

    /* --- a device with one is handed out as a view of itself ------------- */

    KT_CHECK_INT_EQ(k_blockdev_register_cached(&s_cached, false), 0);

    view = k_blockdev_find("cached0");

    KT_CHECK(view != NULL);
    KT_CHECK(view != &s_cached);
    KT_CHECK(view->sector_size == CACHE_SECTOR_SIZE);
    KT_CHECK(view->sectors == CACHE_SECTORS);
    KT_CHECK(view->ops != NULL && view->ops->read != NULL && view->ops->write != NULL);

    if (view == NULL || view->ops == NULL) {
        return;
    }

    /* --- a read of the same sector reaches the device once --------------- */

    s_reads = s_writes = s_erases = s_syncs = 0;

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 3, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 1);
    KT_CHECK_INT_EQ(memcmp(buffer, s_data[3], CACHE_SECTOR_SIZE), 0);

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 3, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 1);

    /* --- a write goes through, and the copy follows it ------------------- */

    for (i = 0; i < CACHE_SECTOR_SIZE; i++) {
        expect[i] = (k_u8_t)(0xA5u ^ (k_u8_t)i);
    }

    KT_CHECK_INT_EQ(view->ops->write(view->ctx, 3, expect, 1), 0);
    KT_CHECK_INT_EQ(s_writes, 1);
    KT_CHECK_INT_EQ(memcmp(s_data[3], expect, CACHE_SECTOR_SIZE), 0);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 3, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 1); /* the copy followed the write */
    KT_CHECK_INT_EQ(memcmp(buffer, expect, CACHE_SECTOR_SIZE), 0);

    /* --- a sync writes what is held back, and keeps the copy ------------ */

    KT_CHECK_INT_EQ(view->ops->sync(view->ctx), 0);
    KT_CHECK_INT_EQ(s_syncs, 1);
    KT_CHECK_INT_EQ(s_writes, 1);

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 3, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 1); /* the medium and the copy agree */
    KT_CHECK_INT_EQ(memcmp(buffer, s_data[3], CACHE_SECTOR_SIZE), 0);

    /* --- an erase drops the cache ---------------------------------------- */

    KT_CHECK_INT_EQ(view->ops->erase(view->ctx, 0, 8), 0);
    KT_CHECK_INT_EQ(s_erases, 1);

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 3, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 2);
    KT_CHECK_INT_EQ(memcmp(buffer, s_data[3], CACHE_SECTOR_SIZE), 0);

    /* --- the cache holds its sectors and evicts the oldest --------------- */

    /* A sector that continues a run is a read ahead: one command of the
     * device fills the cache with the next few sectors of the file. */
    s_reads = 0;

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 10, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 1);

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 11, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 2);

    for (i = 12; i < 11 + K_BLOCKDEV_READ_AHEAD; i++) {
        KT_CHECK_INT_EQ(view->ops->read(view->ctx, (k_u32_t)i, buffer, 1), 0);
        KT_CHECK_INT_EQ(memcmp(buffer, s_data[i], CACHE_SECTOR_SIZE), 0);
    }

    KT_CHECK_INT_EQ(s_reads, 2); /* the rest of the run came from the cache */

    /* The sector after the run starts the next one. */
    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 11 + K_BLOCKDEV_READ_AHEAD, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 3);

    /* A sector out of that order is one sector, and the cache of sixteen
     * boxes keeps the last sixteen of them. */
    s_reads = 0;

    for (i = 30; i <= 30 + 2 * K_BLOCKDEV_CACHE_SECTORS; i += 2) {
        KT_CHECK_INT_EQ(view->ops->read(view->ctx, (k_u32_t)i, buffer, 1), 0);
    }

    KT_CHECK_INT_EQ(s_reads, K_BLOCKDEV_CACHE_SECTORS + 1);

    /* The oldest of the run is gone, the last one is still there. */
    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 30, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, K_BLOCKDEV_CACHE_SECTORS + 2);

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 30 + 2 * K_BLOCKDEV_CACHE_SECTORS, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, K_BLOCKDEV_CACHE_SECTORS + 2);

    /* --- a run longer than the cache is a stream and passes through ------ */

    s_reads = 0;

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 0, run, 8), 0);
    KT_CHECK_INT_EQ(s_reads, 1); /* one call of the device, not eight */
    KT_CHECK_INT_EQ(memcmp(run, s_data[0], sizeof(run)), 0);

    /* --- the view comes out of the table as it went in ------------------- */

    KT_CHECK_INT_EQ(k_blockdev_unregister(view), 0);
    KT_CHECK(k_blockdev_find("cached0") == NULL);

    /* --- a cache that holds the writes back ------------------------------ */

    KT_CHECK_INT_EQ(k_blockdev_register_cached(&s_writeback, true), 0);

    view = k_blockdev_find("wb0");
    KT_CHECK(view != NULL && view != &s_writeback);

    if (view == NULL || view->ops == NULL) {
        return;
    }

    s_reads = s_writes = s_erases = s_syncs = 0;

    /* A write stays in the cache, and a read of it is served from there. */
    for (i = 0; i < CACHE_SECTOR_SIZE; i++) {
        expect[i] = (k_u8_t)(0x5Au + (k_u8_t)i);
    }

    KT_CHECK_INT_EQ(view->ops->write(view->ctx, 40, expect, 1), 0);
    KT_CHECK_INT_EQ(s_writes, 0);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 40, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 0);
    KT_CHECK_INT_EQ(memcmp(buffer, expect, CACHE_SECTOR_SIZE), 0);

    /* A sync writes the run: the sector, and the four next to it. */
    for (i = 1; i < 5; i++) {
        KT_CHECK_INT_EQ(view->ops->write(view->ctx, 40 + (k_u32_t)i, expect, 1), 0);
    }

    KT_CHECK_INT_EQ(s_writes, 0);
    KT_CHECK_INT_EQ(view->ops->sync(view->ctx), 0);
    KT_CHECK_INT_EQ(s_syncs, 1);
    KT_CHECK_INT_EQ(s_writes, 1);
    KT_CHECK_INT_EQ(s_last_sector, 40);
    KT_CHECK_INT_EQ(s_last_count, 5);

    /* --- the sectors of a full cache leave it as one command ------------- */

    s_writes = 0;

    for (i = 45; i < 45 + K_BLOCKDEV_CACHE_SECTORS; i++) {
        KT_CHECK_INT_EQ(view->ops->write(view->ctx, (k_u32_t)i, expect, 1), 0);
    }

    /* The clean sectors of the sync above were evicted first. */
    KT_CHECK_INT_EQ(s_writes, 0);

    /* One more sector has no room but a dirty one, and the run leaves. */
    KT_CHECK_INT_EQ(view->ops->write(view->ctx, 45 + K_BLOCKDEV_CACHE_SECTORS, expect, 1), 0);
    KT_CHECK_INT_EQ(s_writes, 1);
    KT_CHECK_INT_EQ(s_last_sector, 45);
    KT_CHECK_INT_EQ(s_last_count, K_BLOCKDEV_CACHE_SECTORS);

    KT_CHECK_INT_EQ(view->ops->sync(view->ctx), 0);
    KT_CHECK_INT_EQ(s_writes, 2);
    KT_CHECK_INT_EQ(s_last_sector, 45 + K_BLOCKDEV_CACHE_SECTORS);
    KT_CHECK_INT_EQ(s_last_count, 1);

    for (i = 40; i <= 45 + K_BLOCKDEV_CACHE_SECTORS; i++) {
        KT_CHECK_INT_EQ(memcmp(s_data[i], expect, CACHE_SECTOR_SIZE), 0);
    }

    /* --- the device that leaves the table takes nothing with it ---------- */

    KT_CHECK_INT_EQ(view->ops->write(view->ctx, 20, expect, 1), 0);
    KT_CHECK_INT_EQ(s_writes, 2);
    KT_CHECK_INT_EQ(k_blockdev_unregister(view), 0);
    KT_CHECK_INT_EQ(s_writes, 3);
    KT_CHECK(k_blockdev_find("wb0") == NULL);

    /* --- a cache of the size the device asks for -------------------------- */

    KT_CHECK_INT_EQ(k_blockdev_register_cached_sized(&s_sized, true, 0), -1);

    KT_CHECK_INT_EQ(k_blockdev_register_cached_sized(&s_sized, true, 4), 0);

    view = k_blockdev_find("sized0");
    KT_CHECK(view != NULL && view != &s_sized);

    if (view == NULL || view->ops == NULL) {
        return;
    }

    s_reads = s_writes = 0;

    /* Four boxes hold four sectors. The sectors are out of order on purpose:
     * a run of them would be a read ahead, and this is the size of the cache
     * and not the cleverness of the stream. */
    for (i = 0; i < 4; i++) {
        KT_CHECK_INT_EQ(view->ops->read(view->ctx, 30 + (k_u32_t)i * 2, buffer, 1), 0);
    }

    KT_CHECK_INT_EQ(s_reads, 4);

    for (i = 0; i < 4; i++) {
        KT_CHECK_INT_EQ(view->ops->read(view->ctx, 30 + (k_u32_t)i * 2, buffer, 1), 0);
        KT_CHECK_INT_EQ(memcmp(buffer, s_data[30 + i * 2], CACHE_SECTOR_SIZE), 0);
    }

    KT_CHECK_INT_EQ(s_reads, 4); /* the four of them are in the cache */

    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 38, buffer, 1), 0);
    KT_CHECK_INT_EQ(view->ops->read(view->ctx, 30, buffer, 1), 0);
    KT_CHECK_INT_EQ(s_reads, 6); /* the first box was the oldest */

    /* --- what the cache reports to /proc ---------------------------------- */

    {
        struct k_blockdev_cache_stats stats;

        KT_CHECK_INT_EQ(k_blockdev_cache_stats(view, &stats), 0);
        KT_CHECK(stats.present);
        KT_CHECK(stats.live);
        KT_CHECK(stats.write_back);
        KT_CHECK_INT_EQ(stats.capacity, 4);
        KT_CHECK_INT_EQ(stats.valid, 4);
        KT_CHECK_INT_EQ(stats.dirty, 0);
        KT_CHECK_INT_EQ(stats.sectors, CACHE_SECTORS);
        KT_CHECK_INT_EQ(stats.sector_size, CACHE_SECTOR_SIZE);

        /* A write stays in a box and the report says so. */
        KT_CHECK_INT_EQ(view->ops->write(view->ctx, 10, expect, 1), 0);
        KT_CHECK_INT_EQ(k_blockdev_cache_stats(view, &stats), 0);
        KT_CHECK_INT_EQ(stats.valid, 4);
        KT_CHECK_INT_EQ(stats.dirty, 1);

        /* The sync empties the dirty boxes and keeps the copy. */
        KT_CHECK_INT_EQ(view->ops->sync(view->ctx), 0);
        KT_CHECK_INT_EQ(k_blockdev_cache_stats(view, &stats), 0);
        KT_CHECK_INT_EQ(stats.dirty, 0);
        KT_CHECK_INT_EQ(stats.valid, 4);

        /* The device the platform registered answers the same report. */
        KT_CHECK_INT_EQ(k_blockdev_cache_stats(&s_sized, &stats), 0);
        KT_CHECK_INT_EQ(stats.capacity, 4);
    }

    /* A device without a cache is in the table and has no boxes. */
    KT_CHECK_INT_EQ(k_blockdev_register(&s_plain), 0);
    {
        struct k_blockdev_cache_stats stats;

        KT_CHECK_INT_EQ(k_blockdev_cache_stats(&s_plain, &stats), 0);
        KT_CHECK(!stats.present);
        KT_CHECK_INT_EQ(stats.dirty, 0);
        KT_CHECK_INT_EQ(stats.sectors, CACHE_SECTORS);
        KT_CHECK_INT_EQ(k_blockdev_cache_stats(&s_plain, NULL), -1);
    }
    KT_CHECK_INT_EQ(k_blockdev_unregister(&s_plain), 0);

    KT_CHECK_INT_EQ(k_blockdev_unregister(view), 0);
    KT_CHECK(k_blockdev_find("sized0") == NULL);
}
