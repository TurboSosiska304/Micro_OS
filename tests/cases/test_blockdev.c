/*
 * Micro OS — the block devices of the system.
 *
 * A filesystem is a consumer of blocks: MicroFS asks for sector 17 of
 * "flash0" and does not care what stands behind it. The test registers a
 * device of its own and walks the table the way a mount does - by name,
 * and through the calls the filesystem will make.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev.h"

#define TEST_SECTORS 8
#define TEST_SECTOR_SIZE 16

static k_u8_t s_data[TEST_SECTORS][TEST_SECTOR_SIZE];

static int test_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector + count > TEST_SECTORS) {
        return -1;
    }

    memcpy(buf, s_data[sector], count * TEST_SECTOR_SIZE);
    return 0;
}

static int test_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);

    if (sector + count > TEST_SECTORS) {
        return -1;
    }

    memcpy(s_data[sector], buf, count * TEST_SECTOR_SIZE);
    return 0;
}

static const struct k_blockdev_ops s_ops = {
    .read = test_read,
    .write = test_write,
    .erase = NULL,
    .sync = NULL,
};

static const struct k_blockdev s_device = {
    .name = "test0",
    .sector_size = TEST_SECTOR_SIZE,
    .sectors = TEST_SECTORS,
    .ops = &s_ops,
    .ctx = s_data,
};

void test_blockdev(void)
{
    const struct k_blockdev *found;
    k_u8_t written[TEST_SECTOR_SIZE];
    k_u8_t read_back[TEST_SECTOR_SIZE];
    k_size_t i;

    ktest_begin("blockdev");

    /* A device without the calls a filesystem needs is not a device. */
    KT_CHECK_INT_EQ(k_blockdev_register(NULL), -1);

    KT_CHECK_INT_EQ(k_blockdev_register(&s_device), 0);

    found = k_blockdev_find("test0");
    KT_CHECK(found == &s_device);
    KT_CHECK(k_blockdev_find("nope") == NULL);
    KT_CHECK(k_blockdev_find(NULL) == NULL);

    /* The device is in the table, and the table is walkable. */
    KT_CHECK(k_blockdev_count() >= 1);
    if (k_blockdev_count() >= 1) {
        KT_CHECK(k_blockdev_at(0) == &s_device);
    }
    KT_CHECK(k_blockdev_at(k_blockdev_count()) == NULL);

    /* One sector written and read back through the operations. */
    for (i = 0; i < TEST_SECTOR_SIZE; i++) {
        written[i] = (k_u8_t)(i * 3 + 1);
    }

    KT_CHECK_INT_EQ(s_device.ops->write(s_device.ctx, 2, written, 1), 0);
    KT_CHECK_INT_EQ(s_device.ops->read(s_device.ctx, 2, read_back, 1), 0);
    KT_CHECK_INT_EQ(memcmp(written, read_back, TEST_SECTOR_SIZE), 0);

    /* The bounds are the bounds of the device. */
    KT_CHECK_INT_EQ(s_device.ops->read(s_device.ctx, TEST_SECTORS, read_back, 1), -1);
}
