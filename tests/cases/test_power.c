/*
 * Micro OS — the clean end of the machine (ADR 0050).
 *
 * The tests of the file are about the half of a shutdown that has an answer:
 * the sync and the flush. The last step (the port takes the power down)
 * does not return, so a test that reached it would take the process of the
 * whole suite with it; what is checked here is the refusal that keeps the
 * machine alive - the rule of ADR 0041 that the data is worth more than the
 * speed of a shutdown.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/power.h"

#define TEST_SECTORS 8
#define TEST_SECTOR_SIZE 16

static int power_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    K_UNUSED(ctx);
    K_UNUSED(sector);

    memset(buf, 0, count * TEST_SECTOR_SIZE);
    return 0;
}

static int power_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    K_UNUSED(ctx);
    K_UNUSED(sector);
    K_UNUSED(buf);
    K_UNUSED(count);

    return 0;
}

static int s_sync_calls;
static int s_sync_answer;

static int power_sync(void *ctx)
{
    K_UNUSED(ctx);

    s_sync_calls++;
    return s_sync_answer;
}

static const struct k_blockdev_ops s_ops = {
    .read = power_read,
    .write = power_write,
    .erase = NULL,
    .sync = power_sync,
};

static const struct k_blockdev s_device = {
    .name = "power0",
    .sector_size = TEST_SECTOR_SIZE,
    .sectors = TEST_SECTORS,
    .ops = &s_ops,
    .ctx = NULL,
};

/* --- the flush of every disk --------------------------------------------- */

static void test_power_sync_all(void)
{
    int rc;

    s_sync_calls = 0;
    s_sync_answer = 0;

    KT_CHECK_INT_EQ(k_blockdev_register(&s_device), 0);

    rc = k_blockdev_sync_all();

    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK(s_sync_calls >= 1);

    KT_CHECK_INT_EQ(k_blockdev_unregister(&s_device), 0);
}

/*
 * The negative of the whole lane: a disk that does not answer the flush is
 * a machine that stays on. The call of the clean end has to say so before a
 * task is signalled - the pre-flight flush is the one that decides - and
 * the machine has to be running when it returns.
 */
static void test_power_flush_failure_keeps_the_machine_on(void)
{
    int rc;

    s_sync_calls = 0;
    s_sync_answer = -1;

    KT_CHECK_INT_EQ(k_blockdev_register(&s_device), 0);

    rc = k_power_sync();
    KT_CHECK_INT_EQ(rc, KERR_IO);
    KT_CHECK(s_sync_calls >= 1);

    rc = k_power_shutdown(K_POWER_OFF, false);
    KT_CHECK_INT_EQ(rc, KERR_IO);
    KT_CHECK(!k_power_ending());

    KT_CHECK_INT_EQ(k_blockdev_unregister(&s_device), 0);

    s_sync_answer = 0;
}

/* The names the log of the clean end prints. */
static void test_power_action_names(void)
{
    KT_CHECK_STR_EQ(k_power_action_name(K_POWER_HALT), "halt");
    KT_CHECK_STR_EQ(k_power_action_name(K_POWER_OFF), "off");
    KT_CHECK_STR_EQ(k_power_action_name(K_POWER_REBOOT), "reboot");
}

void test_power(void)
{
    test_power_action_names();
    test_power_sync_all();
    test_power_flush_failure_keeps_the_machine_on();
}
