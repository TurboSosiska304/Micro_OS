/*
 * Micro OS — a volume bigger than the map of the free blocks used to be.
 *
 * The map of the free blocks was a static array of two blocks, so a volume
 * stopped at 16 MiB for a reason that had nothing to do with the format: a
 * bit of the map describes a block of 1 KiB, and the 32-bit fields of the
 * header hold a volume of terabytes. The mount now allocates the map the
 * volume it finds needs - 2 KiB for 16 MiB, 8 KiB for 64 MiB, 615 KiB for
 * 5 GiB - and gives it back on the way out.
 *
 * The volume of the test is a sparse file: it is 64 MiB long and holds only
 * the blocks the filesystem writes, so the test costs the memory of a
 * volume of that size in name only. The counter of the allocator is the
 * witness of the memory, and the map of a volume of 80 GiB is refused with
 * a word before a block of the medium is read.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "kernel/kmm.h"
#include "mfs/mfs.h"

/* The volume: 64 MiB in blocks of 1 KiB, the biggest the map of 8 MiB covers. */
#define BIG_BLOCKS 65536u
#define BIG_BYTES ((k_size_t)BIG_BLOCKS * MFS_BLOCK_SIZE)

/* The file of the test: 64 blocks of the chain, a pattern in every byte. */
#define BIG_DATA_BLOCKS 64u
#define BIG_DATA_SIZE (BIG_DATA_BLOCKS * (MFS_BLOCK_SIZE - 4u))

/* A device of 80 GiB: 16 GiB past what the map of the design holds. */
#define CEILING_BLOCKS (80u * 1024u * 1024u)

/* The file of the host behind the volume. */
struct file_device {
    int fd;
    k_u32_t blocks;
    k_u32_t reads;
    k_u32_t writes;
};

/* The device of the ceiling: every call is a problem, it must not be made. */
struct ceiling_device {
    k_u32_t reads;
    k_u32_t writes;
};

static k_u8_t g_written[BIG_DATA_SIZE];
static k_u8_t g_read[BIG_DATA_SIZE + MFS_BLOCK_SIZE];

static int file_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct file_device *dev = (struct file_device *)ctx;
    size_t size = (size_t)count * MFS_BLOCK_SIZE;
    ssize_t got;

    if (sector >= dev->blocks || count > dev->blocks - sector) {
        return -1;
    }

    got = pread(dev->fd, buf, size, (off_t)sector * MFS_BLOCK_SIZE);
    if (got != (ssize_t)size) {
        return -1;
    }

    dev->reads += (k_u32_t)count;

    return 0;
}

static int file_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct file_device *dev = (struct file_device *)ctx;
    size_t size = (size_t)count * MFS_BLOCK_SIZE;
    ssize_t put;

    if (sector >= dev->blocks || count > dev->blocks - sector) {
        return -1;
    }

    put = pwrite(dev->fd, buf, size, (off_t)sector * MFS_BLOCK_SIZE);
    if (put != (ssize_t)size) {
        return -1;
    }

    dev->writes += (k_u32_t)count;

    return 0;
}

static int ceiling_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct ceiling_device *dev = (struct ceiling_device *)ctx;

    (void)sector;
    (void)buf;
    (void)count;
    dev->reads++;

    return -1;
}

static int ceiling_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct ceiling_device *dev = (struct ceiling_device *)ctx;

    (void)sector;
    (void)buf;
    (void)count;
    dev->writes++;

    return -1;
}

static const struct k_blockdev_ops g_file_ops = {
    .read = file_read,
    .write = file_write,
    .erase = NULL,
    .sync = NULL, /* the file of the host is written through */
};

static const struct k_blockdev_ops g_ceiling_ops = {
    .read = ceiling_read,
    .write = ceiling_write,
    .erase = NULL,
    .sync = NULL,
};

/* The byte of a pattern: the block and the place in it are both in it. */
static k_u8_t pattern_at(k_u32_t offset)
{
    return (k_u8_t)((offset / (MFS_BLOCK_SIZE - 4u)) * 29u + (offset % (MFS_BLOCK_SIZE - 4u)) * 7u +
                    11u);
}

/*
 * A volume of 80 GiB is refused for the map, not for the disk: the answer
 * comes before the first block is read, and the memory of the machine has
 * nothing of it afterwards.
 */
static void check_ceiling(void)
{
    struct ceiling_device medium;
    struct k_blockdev device;
    struct kmm_stats before;
    struct kmm_stats after;

    memset(&medium, 0, sizeof(medium));
    memset(&device, 0, sizeof(device));

    device.name = "ceiling";
    device.sector_size = MFS_BLOCK_SIZE;
    device.sectors = CEILING_BLOCKS;
    device.ops = &g_ceiling_ops;
    device.ctx = &medium;

    kmm_get_stats(&before);

    KT_CHECK_INT_EQ(mfs_format(&device, 256), MFS_EINVAL);

    /* The refusal is about the layout of the volume, not about its bytes. */
    KT_CHECK_INT_EQ(medium.reads, 0);
    KT_CHECK_INT_EQ(medium.writes, 0);

    kmm_get_stats(&after);
    KT_CHECK_INT_EQ(after.allocations, before.allocations);
    KT_CHECK_INT_EQ(after.total_bytes, before.total_bytes);
}

void test_mfs_big(void)
{
    struct file_device medium;
    struct k_blockdev device;
    struct kmm_stats before;
    struct kmm_stats mounted;
    struct kmm_stats after;
    struct mfs_info info;
    struct mfs_check_report report;
    struct mfs_file file;
    char path[] = "/tmp/microos-mfs-big-XXXXXX";
    k_u32_t i;
    int fd;
    int rc;

    ktest_begin("mfs: a volume of 64 MiB, a map of 8 KiB");

    check_ceiling();

    fd = mkstemp(path);
    KT_CHECK(fd >= 0);

    if (fd < 0) {
        return;
    }

    /* The file is a name of 64 MiB, the blocks cost the volume only. */
    (void)unlink(path);
    KT_CHECK_INT_EQ(ftruncate(fd, (off_t)BIG_BYTES), 0);

    memset(&medium, 0, sizeof(medium));
    memset(&device, 0, sizeof(device));

    medium.fd = fd;
    medium.blocks = BIG_BLOCKS;
    device.name = "big";
    device.sector_size = MFS_BLOCK_SIZE;
    device.sectors = BIG_BLOCKS;
    device.ops = &g_file_ops;
    device.ctx = &medium;

    kmm_get_stats(&before);

    KT_CHECK_INT_EQ(mfs_format(&device, 256), MFS_OK);

    /* A format is not a mount: the map it built is not held afterwards. */
    kmm_get_stats(&after);
    KT_CHECK_INT_EQ(after.allocations, before.allocations);
    KT_CHECK_INT_EQ(after.total_bytes, before.total_bytes);

    rc = mfs_mount(&device);
    KT_CHECK_INT_EQ(rc, MFS_OK);
    if (rc != MFS_OK) {
        close(fd);
        return;
    }

    /*
     * 64 MiB of data is 8 KiB of the map: one bit per block, rounded up to
     * the blocks of the device, and one byte per block of the map that the
     * journal may have to write (ADR 0047). The mount holds the map, its
     * dirty flags and a rollback snapshot of both so a failed transaction
     * can restore the bitmap without leaking allocated blocks.
     */
    kmm_get_stats(&mounted);
    KT_CHECK_INT_EQ(mounted.allocations, before.allocations + 4);
    KT_CHECK_INT_EQ(mounted.total_bytes, before.total_bytes + 2u * (8u * MFS_BLOCK_SIZE + 8u));

    /* The volume says what it is: 64 MiB, almost all of it data. */
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK_INT_EQ(info.total_blocks, BIG_BLOCKS);
    KT_CHECK_INT_EQ(info.block_size, MFS_BLOCK_SIZE);
    KT_CHECK(info.data_blocks > 60000u);
    KT_CHECK_INT_EQ(info.data_free, info.data_blocks - 1u);
    /*
     * The table of the records is sized by the records and not by the
     * volume (ADR 0038): the format asked for 256 of them and keeps a
     * reserve of 64, so a volume of 64 MiB and a volume of 5 GiB walk the
     * same table of twenty blocks.
     */
    KT_CHECK_INT_EQ(info.inodes_total, 256u + 64u - 1u);
    KT_CHECK(info.meta_blocks < 32u);

    /*
     * The count of the used records is a walk of the table, and the walk
     * costs the table: 16 records of a version 3 volume share a block, so
     * the reads of the walk are the blocks of the table and not the
     * records. Without the cache of the table the walk read the same block
     * once per record, which is 16 times the bytes of the table.
     */
    {
        k_u32_t first = medium.reads;
        k_u32_t blocks;

        KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
        blocks = medium.reads - first;
        KT_CHECK(blocks <= info.inodes_total / 8u + 2u);
    }

    /* A file of 64 blocks, written and read back through the device. */
    for (i = 0; i < BIG_DATA_SIZE; i++) {
        g_written[i] = pattern_at(i);
    }

    KT_CHECK_INT_EQ(mfs_create("/big.bin"), MFS_OK);
    rc = mfs_open("/big.bin", MFS_O_RDWR, &file);
    KT_CHECK_INT_EQ(rc, MFS_OK);
    if (rc != MFS_OK) {
        mfs_unmount();
        close(fd);
        return;
    }

    KT_CHECK_INT_EQ(mfs_write(&file, g_written, BIG_DATA_SIZE), (int)BIG_DATA_SIZE);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    memset(g_read, 0, sizeof(g_read));
    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, BIG_DATA_SIZE), (int)BIG_DATA_SIZE);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(memcmp(g_read, g_written, BIG_DATA_SIZE), 0);

    /* The map of the volume and the records tell the same story. */
    memset(&report, 0, sizeof(report));
    rc = mfs_check(&report);
    KT_CHECK_INT_EQ(rc, MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.data_blocks, info.data_blocks);
    /* The root directory and the 64 blocks of the file. */
    KT_CHECK_INT_EQ(report.used_blocks, BIG_DATA_BLOCKS + 1u);

    /* The map goes back with the volume: nothing of 64 MiB stays. */
    mfs_unmount();
    kmm_get_stats(&after);
    KT_CHECK_INT_EQ(after.allocations, before.allocations);
    KT_CHECK_INT_EQ(after.total_bytes, before.total_bytes);

    /* And a second mount finds the file where the unmount left it. */
    rc = mfs_mount(&device);
    KT_CHECK_INT_EQ(rc, MFS_OK);
    if (rc != MFS_OK) {
        close(fd);
        return;
    }

    memset(g_read, 0, sizeof(g_read));
    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, BIG_DATA_SIZE), (int)BIG_DATA_SIZE);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(memcmp(g_read, g_written, BIG_DATA_SIZE), 0);

    mfs_unmount();
    close(fd);
}
