#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/task.h"
#include "port_host.h"

static void test_alloc_free(void)
{
    struct kmm_stats stats;
    char *block;

    ktest_begin("kmm: a block can be allocated and freed");

    kmm_init();

    block = (char *)kmm_alloc(1024, KMM_PRIORITY_KERNEL);
    KT_CHECK(block != NULL);

    if (block != NULL) {
        memset(block, 0x5A, 1024);
        KT_CHECK_INT_EQ(block[0], 0x5A);
        KT_CHECK_INT_EQ(block[1023], 0x5A);
    }

    kmm_get_stats(&stats);
    KT_CHECK_INT_EQ(stats.allocations, 1);
    KT_CHECK(stats.total_bytes > 0);

    kmm_free(block);

    kmm_get_stats(&stats);
    KT_CHECK_INT_EQ(stats.allocations, 0);
    KT_CHECK_INT_EQ(stats.total_bytes, 0);
}

static void test_priority_pools(void)
{
    struct kmm_stats stats;
    void *kernel_block;
    void *user_block;

    ktest_begin("kmm: the priority decides where the memory comes from");

    kmm_init();

    /* The kernel and the drivers get the internal RAM. */
    kernel_block = kmm_alloc(4096, KMM_PRIORITY_KERNEL);
    KT_CHECK(kernel_block != NULL);

    kmm_get_stats(&stats);
    KT_CHECK(stats.kernel_bytes > 0);
    KT_CHECK_INT_EQ(stats.psram_bytes, 0);

    /* The user gets the PSRAM. */
    user_block = kmm_alloc(4096, KMM_PRIORITY_USER);
    KT_CHECK(user_block != NULL);

    kmm_get_stats(&stats);
    KT_CHECK(stats.kernel_bytes > 0);
    KT_CHECK(stats.psram_bytes > 0);
    KT_CHECK_INT_EQ(stats.user_bytes, 0);

    kmm_free(kernel_block);
    kmm_free(user_block);
}

static void test_system_priority(void)
{
    struct kmm_stats stats;
    void *block;

    ktest_begin("kmm: a system utility gets the internal RAM");

    kmm_init();

    block = kmm_alloc(4096, KMM_PRIORITY_SYSTEM);
    KT_CHECK(block != NULL);

    kmm_get_stats(&stats);
    KT_CHECK(stats.system_bytes > 0);
    KT_CHECK_INT_EQ(stats.psram_bytes, 0);

    kmm_free(block);
}

static void test_task_priority(void)
{
    struct task *self = task_self();
    struct kmm_stats stats;
    void *block;

    ktest_begin("kmm: the priority of the task decides the priority of the memory");

    KT_CHECK(self != NULL);
    if (self == NULL) {
        return;
    }

    kmm_init();

    /* A new task is a user application. */
    KT_CHECK_INT_EQ(task_get_mem_priority(self), KMM_PRIORITY_USER);
    KT_CHECK_INT_EQ(kmm_current_priority(), KMM_PRIORITY_USER);

    /* The system flag raises the priority of the allocations. */
    task_set_mem_priority(self, KMM_PRIORITY_SYSTEM);
    KT_CHECK_INT_EQ(kmm_current_priority(), KMM_PRIORITY_SYSTEM);

    block = kmm_alloc(4096, kmm_current_priority());
    KT_CHECK(block != NULL);

    kmm_get_stats(&stats);
    KT_CHECK(stats.system_bytes > 0);
    KT_CHECK_INT_EQ(stats.psram_bytes, 0);

    kmm_free(block);

    /* And back to the user. */
    task_set_mem_priority(self, KMM_PRIORITY_USER);
    KT_CHECK_INT_EQ(kmm_current_priority(), KMM_PRIORITY_USER);
}

static void test_alignment(void)
{
    void *a;
    void *b;

    ktest_begin("kmm: the blocks are aligned");

    kmm_init();

    a = kmm_alloc(13, KMM_PRIORITY_KERNEL);
    b = kmm_alloc(13, KMM_PRIORITY_KERNEL);

    KT_CHECK(a != NULL);
    KT_CHECK(b != NULL);

    if (a != NULL && b != NULL) {
        KT_CHECK_INT_EQ((k_size_t)a % 8, 0);
        KT_CHECK_INT_EQ((k_size_t)b % 8, 0);
    }

    kmm_free(a);
    kmm_free(b);
}

void test_kmm(void)
{
    test_alloc_free();
    test_priority_pools();
    test_system_priority();
    test_task_priority();
    test_alignment();
}
