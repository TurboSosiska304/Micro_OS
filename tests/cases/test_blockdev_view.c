/*
 * Micro OS — the grain of a volume over the grain of a medium (ADR 0042).
 *
 * The sector of a device is a fact of the medium; the grain of a volume is a
 * fact of its format. The view is the device of the second: the sector of the
 * volume over the sector of the medium, one call of the block layer per block
 * of the volume and one command of the medium per run.
 *
 * The test registers a medium of its own, opens a view over it and counts what
 * the medium was asked: the sectors of a view are the sectors of the medium
 * multiplied, in the order of the call, and nothing else changes.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev.h"

#define VIEW_SECTORS 8
#define VIEW_SECTOR_SIZE 16
#define VIEW_GRAIN 64u /* four sectors of the medium */

static k_u8_t s_medium[VIEW_SECTORS * VIEW_SECTOR_SIZE];
static k_u32_t s_reads;
static k_u32_t s_writes;
static k_u32_t s_read_sectors;
static k_u32_t s_write_sectors;
static k_u32_t s_syncs;

static int medium_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= VIEW_SECTORS || count > (k_size_t)VIEW_SECTORS - sector) {
        return -1;
    }

    s_reads++;
    s_read_sectors += (k_u32_t)count;
    memcpy(buf, s_medium + (k_size_t)sector * VIEW_SECTOR_SIZE, count * VIEW_SECTOR_SIZE);

    return 0;
}

static int medium_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= VIEW_SECTORS || count > (k_size_t)VIEW_SECTORS - sector) {
        return -1;
    }

    s_writes++;
    s_write_sectors += (k_u32_t)count;
    memcpy(s_medium + (k_size_t)sector * VIEW_SECTOR_SIZE, buf, count * VIEW_SECTOR_SIZE);

    return 0;
}

static int medium_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector >= VIEW_SECTORS || count > (k_size_t)VIEW_SECTORS - sector) {
        return -1;
    }

    memset(s_medium + (k_size_t)sector * VIEW_SECTOR_SIZE, 0, count * VIEW_SECTOR_SIZE);

    return 0;
}

static int medium_sync(void *ctx)
{
    K_UNUSED(ctx);

    s_syncs++;

    return 0;
}

static const struct k_blockdev_ops s_medium_ops = {
    .read = medium_read,
    .write = medium_write,
    .erase = medium_erase,
    .sync = medium_sync,
};

/* A medium without an erase and without a sync: the view names neither. */
static const struct k_blockdev_ops s_plain_ops = {
    .read = medium_read,
    .write = medium_write,
    .erase = NULL,
    .sync = NULL,
};

static struct k_blockdev s_medium_device = {
    .name = "view0",
    .sector_size = VIEW_SECTOR_SIZE,
    .sectors = VIEW_SECTORS,
    .ops = &s_medium_ops,
    .ctx = NULL,
};

static void counters_reset(void)
{
    s_reads = 0;
    s_writes = 0;
    s_read_sectors = 0;
    s_write_sectors = 0;
    s_syncs = 0;
}

void test_blockdev_view(void)
{
    struct k_blockdev_view view;
    const struct k_blockdev *dev;
    k_u8_t out[2u * VIEW_GRAIN];
    k_u8_t in[2u * VIEW_GRAIN];
    k_size_t i;
    k_size_t devices;

    ktest_begin("blockdev: a view of a medium at the grain of a volume");

    memset(s_medium, 0, sizeof(s_medium));
    memset(out, 0, sizeof(out));
    counters_reset();

    s_medium_device.sector_size = VIEW_SECTOR_SIZE;
    s_medium_device.sectors = VIEW_SECTORS;
    s_medium_device.ops = &s_medium_ops;

    /* --- what a view refuses ------------------------------------------- */

    KT_CHECK_INT_EQ(k_blockdev_view_open(NULL, &s_medium_device, VIEW_GRAIN), -1);
    KT_CHECK_INT_EQ(k_blockdev_view_open(&view, NULL, VIEW_GRAIN), -1);

    /* The grain of the medium is the medium, and a sector of the medium is
     * not a fraction of a sector of the view. */
    KT_CHECK_INT_EQ(k_blockdev_view_open(&view, &s_medium_device, VIEW_SECTOR_SIZE), -1);
    KT_CHECK_INT_EQ(k_blockdev_view_open(&view, &s_medium_device, VIEW_GRAIN - 8u), -1);
    KT_CHECK_INT_EQ(k_blockdev_view_open(&view, &s_medium_device, 0u), -1);

    /* A medium shorter than one sector of the view has no view of it. */
    s_medium_device.sectors = 3;
    KT_CHECK_INT_EQ(k_blockdev_view_open(&view, &s_medium_device, VIEW_GRAIN), -1);
    s_medium_device.sectors = VIEW_SECTORS;

    /* A device without the two calls of a filesystem is not a medium. */
    s_medium_device.ops = NULL;
    KT_CHECK_INT_EQ(k_blockdev_view_open(&view, &s_medium_device, VIEW_GRAIN), -1);
    s_medium_device.ops = &s_medium_ops;

    /* --- the view of a medium of 8 sectors of 16 bytes ------------------ */

    KT_CHECK_INT_EQ(k_blockdev_view_open(&view, &s_medium_device, VIEW_GRAIN), 0);

    dev = k_blockdev_view_device(&view);
    KT_CHECK(dev != NULL);
    KT_CHECK_INT_EQ((int)dev->sector_size, (int)VIEW_GRAIN);
    KT_CHECK_INT_EQ((int)dev->sectors, (int)(VIEW_SECTORS * VIEW_SECTOR_SIZE / VIEW_GRAIN));

    /* The view is the volume's device and not an entry of the table: the
     * shell and /proc list the machine's devices and not the grain of a
     * filesystem. */
    devices = k_blockdev_count();
    KT_CHECK(k_blockdev_find("view0") == NULL);
    KT_CHECK_INT_EQ((int)k_blockdev_count(), (int)devices);

    /* Two sectors of the view are the eight sectors of the medium, in the
     * order of the call, and the medium hears one call and not eight. */
    for (i = 0; i < sizeof(out); i++) {
        out[i] = (k_u8_t)(i * 7u + 3u);
    }

    KT_CHECK_INT_EQ(dev->ops->write(dev->ctx, 0, out, 2), 0);
    KT_CHECK_INT_EQ((int)s_writes, 1);
    KT_CHECK_INT_EQ((int)s_write_sectors, VIEW_SECTORS);

    for (i = 0; i < sizeof(s_medium); i++) {
        KT_CHECK_INT_EQ(s_medium[i], (k_u8_t)(i * 7u + 3u));
    }

    memset(in, 0, sizeof(in));
    KT_CHECK_INT_EQ(dev->ops->read(dev->ctx, 0, in, 2), 0);
    KT_CHECK_INT_EQ(memcmp(in, out, sizeof(out)), 0);
    KT_CHECK_INT_EQ((int)s_reads, 1);
    KT_CHECK_INT_EQ((int)s_read_sectors, VIEW_SECTORS);

    /* The second sector of the view is the second half of the medium. */
    KT_CHECK_INT_EQ(dev->ops->read(dev->ctx, 1, in, 1), 0);
    KT_CHECK_INT_EQ(memcmp(in, out + VIEW_GRAIN, VIEW_GRAIN), 0);

    /* The end of the view is the end of the medium, rounded down to a whole
     * sector of the view. */
    KT_CHECK_INT_EQ(dev->ops->read(dev->ctx, 2, in, 1), -1);
    KT_CHECK_INT_EQ(dev->ops->read(dev->ctx, 1, in, 2), -1);
    KT_CHECK_INT_EQ(dev->ops->write(dev->ctx, 2, out, 1), -1);

    /* The erase and the sync of the medium are the erase and the sync of the
     * view, with the sectors multiplied. */
    KT_CHECK(dev->ops->erase != NULL);
    KT_CHECK(dev->ops->sync != NULL);

    if (dev->ops->erase != NULL) {
        KT_CHECK_INT_EQ(dev->ops->erase(dev->ctx, 1, 1), 0);

        for (i = 0; i < VIEW_GRAIN; i++) {
            KT_CHECK_INT_EQ(s_medium[VIEW_GRAIN + i], 0);
        }

        KT_CHECK_INT_EQ(s_medium[0], (k_u8_t)3u); /* the sector in front is kept */
    }

    if (dev->ops->sync != NULL) {
        KT_CHECK_INT_EQ(dev->ops->sync(dev->ctx), 0);
        KT_CHECK_INT_EQ((int)s_syncs, 1);
    }

    /* --- a medium without an erase and a sync names neither ------------ */
    s_medium_device.ops = &s_plain_ops;
    KT_CHECK_INT_EQ(k_blockdev_view_open(&view, &s_medium_device, VIEW_GRAIN), 0);
    KT_CHECK(dev->ops->erase == NULL);
    KT_CHECK(dev->ops->sync == NULL);

    /* --- the view is forgotten with the volume ------------------------- */
    k_blockdev_view_close(&view);
    KT_CHECK(view.dev.ops == NULL);
    KT_CHECK(view.medium == NULL);

    /* ... and the structure of the medium is untouched by all of it. */
    KT_CHECK_INT_EQ((int)s_medium_device.sector_size, (int)VIEW_SECTOR_SIZE);
    KT_CHECK_INT_EQ((int)s_medium_device.sectors, (int)VIEW_SECTORS);
}
