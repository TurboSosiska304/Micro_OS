#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_host.h"

#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

#define MAX_SIZES 256
#define SLAB_BLOCKS 256
#define SLAB_SIZE 64

/* The sizes of the blocks: they come from the real numbers of the corpus. */
static k_size_t g_sizes[MAX_SIZES];
static int g_size_count;

static void load_sizes(void)
{
    FILE *file = fopen(KTEST_CORPUS, "r");
    char line[128];

    g_size_count = 0;

    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL && g_size_count < MAX_SIZES) {
        size_t len = strlen(line);
        long long value;
        char *end = NULL;

        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        value = strtoll(line, &end, 10);
        if (end == line || (end != NULL && *end != '\0')) {
            continue;
        }

        /* The size of the block: the real value, brought into the range
         * where the small and the big blocks live together. */
        if (value < 0) {
            value = -value;
        }
        g_sizes[g_size_count++] = (k_size_t)(value % 600) + 1;
    }

    fclose(file);
}

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

/*
 * The property: the block keeps the data that was written into it, whatever
 * the size of the block is.
 */
static void test_blocks_keep_the_data(void)
{
    struct kmm_stats stats;
    void *blocks[MAX_SIZES];
    int i;

    ktest_begin("kmm: the block keeps the data, whatever the size");

    kmm_init();
    load_sizes();
    KT_CHECK(g_size_count > 0);

    for (i = 0; i < g_size_count; i++) {
        k_size_t size = g_sizes[i];
        char *block = (char *)kmm_alloc(size, KMM_PRIORITY_KERNEL);

        KT_CHECK(block != NULL);
        if (block == NULL) {
            continue;
        }

        blocks[i] = block;

        /* The pattern is the size of the block itself. */
        memset(block, (int)(size & 0xFF), size);
        KT_CHECK_INT_EQ((k_size_t)block % 8, 0);
    }

    for (i = 0; i < g_size_count; i++) {
        k_size_t size = g_sizes[i];
        char *block = (char *)blocks[i];

        if (block == NULL) {
            continue;
        }

        KT_CHECK_INT_EQ((unsigned char)block[0], (unsigned char)(size & 0xFF));
        KT_CHECK_INT_EQ((unsigned char)block[size / 2], (unsigned char)(size & 0xFF));
        KT_CHECK_INT_EQ((unsigned char)block[size - 1], (unsigned char)(size & 0xFF));
    }

    for (i = 0; i < g_size_count; i++) {
        kmm_free(blocks[i]);
    }

    kmm_get_stats(&stats);
    KT_CHECK_INT_EQ(stats.allocations, 0);
    KT_CHECK_INT_EQ(stats.total_bytes, 0);
}

/*
 * The property: the blocks do not overlap. The pattern of one block never
 * appears in another one.
 */
static void test_blocks_do_not_overlap(void)
{
    void *blocks[32];
    int i;
    int j;

    ktest_begin("kmm: the blocks do not overlap");

    kmm_init();

    for (i = 0; i < 32; i++) {
        blocks[i] = kmm_alloc(32, KMM_PRIORITY_KERNEL);
        KT_CHECK(blocks[i] != NULL);
        if (blocks[i] != NULL) {
            memset(blocks[i], i + 1, 32);
        }
    }

    for (i = 0; i < 32; i++) {
        if (blocks[i] == NULL) {
            continue;
        }
        for (j = 0; j < 32; j++) {
            KT_CHECK_INT_EQ((unsigned char)((char *)blocks[i])[j], i + 1);
        }
    }

    for (i = 0; i < 32; i++) {
        kmm_free(blocks[i]);
    }
}

/*
 * The property: the small blocks come from the slabs, and the freed slabs
 * are used again - the memory is not wasted.
 */
static void test_slabs_are_reused(void)
{
    struct kmm_stats stats;
    void *blocks[SLAB_BLOCKS];
    k_size_t slabs_before;
    int i;

    ktest_begin("kmm: the slabs are reused, the memory is not wasted");

    kmm_init();

    for (i = 0; i < SLAB_BLOCKS; i++) {
        blocks[i] = kmm_alloc(SLAB_SIZE, KMM_PRIORITY_KERNEL);
        KT_CHECK(blocks[i] != NULL);
    }

    kmm_get_stats(&stats);
    KT_CHECK(stats.slabs > 0);

    /* 256 blocks of 64 bytes fit into a few slabs, not into 256. */
    KT_CHECK(stats.slabs <= 8);

    slabs_before = stats.slabs;

    for (i = 0; i < SLAB_BLOCKS; i++) {
        kmm_free(blocks[i]);
    }

    /* The same blocks again: no new slab is needed. */
    for (i = 0; i < SLAB_BLOCKS; i++) {
        blocks[i] = kmm_alloc(SLAB_SIZE, KMM_PRIORITY_KERNEL);
        KT_CHECK(blocks[i] != NULL);
    }

    kmm_get_stats(&stats);
    KT_CHECK_INT_EQ(stats.slabs, slabs_before);

    for (i = 0; i < SLAB_BLOCKS; i++) {
        kmm_free(blocks[i]);
    }
}

/*
 * The property: the small blocks follow the same rule as the big ones - the
 * priority decides where the memory comes from.
 */
static void test_small_blocks_follow_the_priority(void)
{
    struct kmm_stats stats;
    void *user_block;
    void *kernel_block;

    ktest_begin("kmm: the small blocks follow the priority of the memory");

    kmm_init();

    user_block = kmm_alloc(100, KMM_PRIORITY_USER);
    kernel_block = kmm_alloc(100, KMM_PRIORITY_KERNEL);

    KT_CHECK(user_block != NULL);
    KT_CHECK(kernel_block != NULL);

    kmm_get_stats(&stats);
    KT_CHECK(stats.psram_bytes > 0);  /* the user block came from the PSRAM */
    KT_CHECK(stats.kernel_bytes > 0); /* the kernel block from the internal RAM */
    KT_CHECK_INT_EQ(stats.user_bytes, 0);

    kmm_free(user_block);
    kmm_free(kernel_block);

    kmm_get_stats(&stats);
    KT_CHECK_INT_EQ(stats.allocations, 0);
    KT_CHECK_INT_EQ(stats.psram_bytes, 0);
    KT_CHECK_INT_EQ(stats.kernel_bytes, 0);
}

/*
 * The property: the big blocks do not come from the slabs and they work the
 * same way.
 */
static void test_large_blocks(void)
{
    struct kmm_stats stats;
    char *block;

    ktest_begin("kmm: the big blocks work like the small ones");

    kmm_init();

    block = (char *)kmm_alloc(4096, KMM_PRIORITY_KERNEL);
    KT_CHECK(block != NULL);

    if (block != NULL) {
        memset(block, 0x33, 4096);
        KT_CHECK_INT_EQ((unsigned char)block[0], 0x33);
        KT_CHECK_INT_EQ((unsigned char)block[4095], 0x33);
        KT_CHECK_INT_EQ((k_size_t)block % 8, 0);
    }

    kmm_get_stats(&stats);
    KT_CHECK(stats.kernel_bytes >= 4096);

    kmm_free(block);

    kmm_get_stats(&stats);
    KT_CHECK_INT_EQ(stats.allocations, 0);
    KT_CHECK_INT_EQ(stats.total_bytes, 0);
}

/*
 * Two tasks in the allocator at the same time: without the lock of kmm
 * they take the same free slot, one block serves two owners, and the
 * patterns painted on the blocks disagree. The paint and the check run
 * outside of the lock on purpose: only the allocator is guarded, and a
 * block of one task has to stay its own while the other task paints
 * its own block next door.
 */
static volatile int g_hammer_failed;
static volatile int g_hammer_done;

static void hammer_task(void *arg)
{
    int id = (int)(intptr_t)arg;
    uint8_t tag = (uint8_t)(0x41 + id);
    int i;

    for (i = 0; i < 100; i++) {
        k_size_t size = (k_size_t)(16 + ((i * 7 + id * 31) % 480));
        uint8_t *block = (uint8_t *)kmm_alloc(size, KMM_PRIORITY_SYSTEM);
        k_size_t j;

        if (block == NULL) {
            g_hammer_failed++;
            continue;
        }

        memset(block, tag, size);
        k_sched_sleep_ms(1);

        for (j = 0; j < size; j++) {
            if (block[j] != tag) {
                g_hammer_failed++;
                break;
            }
        }

        kmm_free(block);
    }

    g_hammer_done++;
}

static void test_two_tasks_alloc(void)
{
    struct task *tasks[2];
    int id;

    ktest_begin("kmm: two tasks that allocate at the same time keep their blocks apart");

    g_hammer_failed = 0;
    g_hammer_done = 0;

    for (id = 0; id < 2; id++) {
        char name[16];

        snprintf(name, sizeof(name), "hammer%d", id);
        tasks[id] = task_create(name, hammer_task, (void *)(intptr_t)id, (k_u8_t)(5 + id));
        KT_CHECK(tasks[id] != NULL);
    }

    for (id = 0; id < 2; id++) {
        if (tasks[id] != NULL) {
            /* The task is waited for: a finished task has to be taken back. */
            KT_CHECK_INT_EQ(task_wait(tasks[id]->pid, NULL, 5000), 0);
        }
    }

    KT_CHECK_INT_EQ(g_hammer_done, 2);
    KT_CHECK_INT_EQ(g_hammer_failed, 0);
}

void test_kmm(void)
{
    test_alloc_free();
    test_priority_pools();
    test_system_priority();
    test_task_priority();
    test_alignment();
    test_blocks_keep_the_data();
    test_blocks_do_not_overlap();
    test_slabs_are_reused();
    test_small_blocks_follow_the_priority();
    test_large_blocks();
    test_two_tasks_alloc();
}
