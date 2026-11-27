/*
 * Micro OS — the test-only fault bus of the block layer.
 *
 * The filesystem sweeps prove recovery; this case proves the injector itself.
 * A successful sync is the only event that moves volatile bytes to stable,
 * and a reordered write is drained by a barrier rather than escaping fsync.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev_fault.h"
#include "kernel/errno.h"

#define FAULT_SECTORS 64u
#define FAULT_SECTOR_SIZE 32u
#define FAULT_BYTES ((k_size_t)FAULT_SECTORS * FAULT_SECTOR_SIZE)

struct fault_medium {
    k_u8_t data[FAULT_SECTORS][FAULT_SECTOR_SIZE];
    k_u32_t order[256];
    k_size_t orders;
};

static struct fault_medium g_medium;
static k_u8_t g_stable[FAULT_BYTES];
static struct k_blockdev g_device;
static struct k_blockdev_fault g_fault;

static int memory_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct fault_medium *medium = ctx;

    if (sector >= FAULT_SECTORS || count > FAULT_SECTORS - sector) {
        return KERR_INVAL;
    }

    memcpy(buf, medium->data[sector], count * FAULT_SECTOR_SIZE);

    return 0;
}

static int memory_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct fault_medium *medium = ctx;

    if (sector >= FAULT_SECTORS || count > FAULT_SECTORS - sector) {
        return KERR_INVAL;
    }

    if (medium->orders < sizeof(medium->order) / sizeof(medium->order[0])) {
        medium->order[medium->orders++] = sector;
    }

    memcpy(medium->data[sector], buf, count * FAULT_SECTOR_SIZE);

    return 0;
}

static const struct k_blockdev_ops g_memory_ops = {
    .read = memory_read,
    .write = memory_write,
};

static void sector_fill(k_u8_t *sector, k_u8_t value)
{
    memset(sector, value, FAULT_SECTOR_SIZE);
}

void test_blockdev_fault(void)
{
    static const int errors[] = {KERR_IO, KERR_NOSPC, KERR_TIMEOUT};
    k_u8_t first[FAULT_SECTOR_SIZE];
    k_u8_t second[FAULT_SECTOR_SIZE];
    k_u8_t back[FAULT_SECTOR_SIZE];
    struct k_blockdev_fault_plan plan = {0};
    k_size_t i;

    ktest_begin("blockdev-fault");

    memset(&g_medium, 0, sizeof(g_medium));
    memset(g_stable, 0, sizeof(g_stable));
    g_device.name = "fault-unit";
    g_device.sector_size = FAULT_SECTOR_SIZE;
    g_device.sectors = FAULT_SECTORS;
    g_device.ops = &g_memory_ops;
    g_device.ctx = &g_medium;

    KT_CHECK_INT_EQ(
        k_blockdev_fault_open(&g_fault, &g_device, g_medium.data[0], g_stable, FAULT_BYTES), 0);
    KT_CHECK(g_fault.dev.ops != NULL && g_fault.dev.ctx == &g_fault);

    sector_fill(first, 0xA5u);
    sector_fill(second, 0x5Au);

    /* A write is volatile until a barrier commits it. */
    KT_CHECK_INT_EQ(k_blockdev_fault_arm(&g_fault, &plan), 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->write(g_fault.dev.ctx, 7, first, 1), 0);
    KT_CHECK(memcmp(g_medium.data[7], first, sizeof(first)) == 0);
    KT_CHECK(memcmp(g_stable + (k_size_t)7 * FAULT_SECTOR_SIZE, first, sizeof(first)) != 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->sync(g_fault.dev.ctx), 0);
    KT_CHECK(memcmp(g_stable + (k_size_t)7 * FAULT_SECTOR_SIZE, first, sizeof(first)) == 0);
    KT_CHECK_INT_EQ(g_fault.stats.commits, 1);

    KT_CHECK_INT_EQ(g_fault.dev.ops->write(g_fault.dev.ctx, 7, second, 1), 0);
    k_blockdev_fault_reboot(&g_fault);
    KT_CHECK_INT_EQ(g_fault.dev.ops->read(g_fault.dev.ctx, 7, back, 1), 0);
    KT_CHECK(memcmp(back, first, sizeof(first)) == 0);

    /* The three errors are returned unchanged by the bus and change no byte. */
    for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        plan.modes = K_BLOCKDEV_FAULT_ERROR;
        plan.at = 1;
        plan.error = errors[i];
        KT_CHECK_INT_EQ(k_blockdev_fault_arm(&g_fault, &plan), 0);
        KT_CHECK_INT_EQ(g_fault.dev.ops->write(g_fault.dev.ctx, 7, second, 1), errors[i]);
        KT_CHECK(memcmp(g_medium.data[7], first, sizeof(first)) == 0);
        KT_CHECK_INT_EQ(g_fault.stats.operations, 1);
        KT_CHECK_INT_EQ(g_fault.stats.injected, 1);
    }

    /* A lost write lies successfully: the caller sees zero, the medium does
     * not receive the byte. */
    plan.modes = K_BLOCKDEV_FAULT_DROP_WRITE;
    plan.at = 1;
    KT_CHECK_INT_EQ(k_blockdev_fault_arm(&g_fault, &plan), 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->write(g_fault.dev.ctx, 7, second, 1), 0);
    KT_CHECK(memcmp(g_medium.data[7], first, sizeof(first)) == 0);
    KT_CHECK_INT_EQ(g_fault.stats.dropped, 1);

    /* The cut happens after the command: volatile has the new byte, stable
     * keeps the old one, and the dead bus answers no further command. */
    plan.modes = K_BLOCKDEV_FAULT_POWER_CUT;
    plan.at = 1;
    KT_CHECK_INT_EQ(k_blockdev_fault_arm(&g_fault, &plan), 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->write(g_fault.dev.ctx, 7, second, 1), 0);
    KT_CHECK(g_fault.power_lost);
    KT_CHECK(memcmp(g_medium.data[7], second, sizeof(second)) == 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->read(g_fault.dev.ctx, 7, back, 1), KERR_TIMEOUT);
    k_blockdev_fault_reboot(&g_fault);
    KT_CHECK(!g_fault.power_lost);
    KT_CHECK(memcmp(g_medium.data[7], first, sizeof(first)) == 0);

    /* Two writes of one sector arrive in reverse order at the medium. */
    plan.modes = K_BLOCKDEV_FAULT_REORDER;
    plan.first = 1;
    plan.second = 2;
    KT_CHECK_INT_EQ(k_blockdev_fault_arm(&g_fault, &plan), 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->write(g_fault.dev.ctx, 9, first, 1), 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->write(g_fault.dev.ctx, 9, second, 1), 0);
    KT_CHECK(memcmp(g_medium.data[9], first, sizeof(first)) == 0);
    KT_CHECK_INT_EQ(g_fault.stats.reordered, 1);
    KT_CHECK_INT_EQ(k_blockdev_fault_kind_at(&g_fault, 1), K_BLOCKDEV_FAULT_OP_WRITE);
    KT_CHECK_INT_EQ(k_blockdev_fault_kind_at(&g_fault, 2), K_BLOCKDEV_FAULT_OP_WRITE);

    /* A barrier cannot be jumped by the held first write. The sync commits it
     * and only then does the power go. */
    plan.modes = K_BLOCKDEV_FAULT_REORDER | K_BLOCKDEV_FAULT_POWER_CUT;
    plan.at = 2;
    KT_CHECK_INT_EQ(k_blockdev_fault_arm(&g_fault, &plan), 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->write(g_fault.dev.ctx, 10, second, 1), 0);
    KT_CHECK_INT_EQ(g_fault.dev.ops->sync(g_fault.dev.ctx), 0);
    KT_CHECK(g_fault.power_lost);
    KT_CHECK_INT_EQ(g_fault.stats.reordered, 1);
    KT_CHECK_INT_EQ(g_fault.stats.commits, 1);
    KT_CHECK_INT_EQ(k_blockdev_fault_kind_at(&g_fault, 2), K_BLOCKDEV_FAULT_OP_SYNC);
    k_blockdev_fault_reboot(&g_fault);
    KT_CHECK_INT_EQ(g_fault.dev.ops->read(g_fault.dev.ctx, 10, back, 1), 0);
    KT_CHECK(memcmp(back, second, sizeof(second)) == 0);

    KT_CHECK_INT_EQ(k_blockdev_fault_kind_at(&g_fault, 0), 0);
    KT_CHECK_INT_EQ(k_blockdev_fault_arm(&g_fault, NULL), -1);

    k_blockdev_fault_close(&g_fault);
}
