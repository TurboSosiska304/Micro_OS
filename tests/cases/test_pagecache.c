#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/pagecache.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

#define PC_TEST_BLOCKS 256
#define PC_SECTOR_SIZE 1024

static struct mfs_ram_device g_volume;
static struct k_blockdev g_ramdev;
static struct k_blockdev g_dev;
static k_u32_t g_read_calls;
static k_u32_t g_write_calls;
static bool g_fail_reads;
static bool g_fail_writes;
static k_u8_t g_priv1[VFS_FILE_DATA_SIZE];

static int pc_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    const struct k_blockdev *ram = (const struct k_blockdev *)ctx;

    if (g_fail_reads) {
        return -1;
    }

    g_read_calls++;
    return ram->ops->read(ram->ctx, sector, buf, count);
}

static int pc_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    const struct k_blockdev *ram = (const struct k_blockdev *)ctx;

    if (g_fail_writes) {
        return -1;
    }

    g_write_calls++;
    return ram->ops->write(ram->ctx, sector, buf, count);
}

static int pc_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    const struct k_blockdev *ram = (const struct k_blockdev *)ctx;

    return ram->ops->erase(ram->ctx, sector, count);
}

static int pc_sync(void *ctx)
{
    const struct k_blockdev *ram = (const struct k_blockdev *)ctx;

    if (ram->ops->sync != NULL) {
        return ram->ops->sync(ram->ctx);
    }
    return 0;
}

static const struct k_blockdev_ops g_ops = {
    .read = pc_read,
    .write = pc_write,
    .erase = pc_erase,
    .sync = pc_sync,
};

static void setup(void)
{
    pagecache_reset();
    g_read_calls = 0;
    g_write_calls = 0;
    g_fail_reads = false;
    g_fail_writes = false;

    mfs_ram_init(&g_volume, PC_TEST_BLOCKS);
    mfs_ram_device(&g_volume, &g_ramdev);

    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.name = "pc0";
    g_dev.sector_size = g_ramdev.sector_size;
    g_dev.sectors = g_ramdev.sectors;
    g_dev.ops = &g_ops;
    g_dev.ctx = &g_ramdev;

    mfs_format(&g_dev, 32);
    mfs_mount(&g_dev);
    mfs_vfs_register("/");
}

static void teardown(void)
{
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
    pagecache_reset();
}

static void test_miss_hit(void)
{
    struct vfs_file file;
    k_u8_t data[PAGECACHE_PAGE_SIZE];
    k_u8_t buf[PAGECACHE_PAGE_SIZE];
    struct pagecache_stats st;
    k_u64_t hits0, misses0;
    int rc;

    ktest_begin("pagecache: miss then hit");

    setup();
    memset(data, 0x5A, sizeof(data));

    rc = vfs_open("/a.bin", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, g_priv1);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK_INT_EQ(vfs_write(&file, data, sizeof(data)), (int)sizeof(data));
    KT_CHECK_INT_EQ(vfs_fsync(&file), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    pagecache_drop_all();
    pagecache_get_stats(&st);
    misses0 = st.misses;
    hits0 = st.hits;

    rc = vfs_open("/a.bin", VFS_O_RDONLY, &file, g_priv1);
    KT_CHECK_INT_EQ(rc, 0);
    memset(buf, 0, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
    KT_CHECK(memcmp(buf, data, sizeof(data)) == 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    pagecache_get_stats(&st);
    KT_CHECK(st.misses > misses0);
    KT_CHECK_INT_EQ(st.hits, hits0);

    misses0 = st.misses;
    hits0 = st.hits;

    rc = vfs_open("/a.bin", VFS_O_RDONLY, &file, g_priv1);
    KT_CHECK_INT_EQ(rc, 0);
    memset(buf, 0, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
    KT_CHECK(memcmp(buf, data, sizeof(data)) == 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    pagecache_get_stats(&st);
    KT_CHECK(st.hits > hits0);
    KT_CHECK_INT_EQ(st.misses, misses0);

    teardown();
}

static volatile int g_shared_step;
static volatile int g_shared_ok;

static void shared_writer(void *arg)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t data[PAGECACHE_PAGE_SIZE];

    K_UNUSED(arg);
    memset(data, 0x11, sizeof(data));

    if (vfs_open("/shared.bin", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv) == 0) {
        vfs_write(&file, data, sizeof(data));
        vfs_fsync(&file);
        vfs_close(&file);
    }

    g_shared_step = 1;
    while (g_shared_step == 1) {
        k_sched_yield();
    }

    if (vfs_open("/shared.bin", VFS_O_RDWR, &file, priv) == 0) {
        memset(data, 0x22, sizeof(data));
        vfs_write(&file, data, sizeof(data));
        vfs_fsync(&file);
        vfs_close(&file);
    }
    g_shared_step = 3;
}

static void test_shared(void)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t buf[PAGECACHE_PAGE_SIZE];
    struct pagecache_stats st;
    k_u64_t misses0, hits0;
    struct task *t;

    ktest_begin("pagecache: two tasks share one page");

    setup();
    g_shared_step = 0;
    g_shared_ok = 0;

    t = task_create("pcw", shared_writer, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(t != NULL);

    while (g_shared_step == 0) {
        k_sched_yield();
    }

    /* First reader loads page - miss. */
    pagecache_get_stats(&st);
    misses0 = st.misses;
    hits0 = st.hits;
    KT_CHECK_INT_EQ(vfs_open("/shared.bin", VFS_O_RDONLY, &file, priv), 0);
    memset(buf, 0, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
    for (int i = 0; i < 16; i++) {
        KT_CHECK_INT_EQ(buf[i], 0x11);
    }
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    pagecache_get_stats(&st);
    KT_CHECK(st.misses >= misses0);

    /* Second read should hit. */
    misses0 = st.misses;
    hits0 = st.hits;
    KT_CHECK_INT_EQ(vfs_open("/shared.bin", VFS_O_RDONLY, &file, priv), 0);
    memset(buf, 0, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    pagecache_get_stats(&st);
    KT_CHECK(st.hits > hits0);
    KT_CHECK_INT_EQ(st.misses, misses0);

    /* Let writer change content and sync. */
    g_shared_step = 2;
    while (g_shared_step != 3) {
        k_sched_yield();
    }

    KT_CHECK_INT_EQ(vfs_open("/shared.bin", VFS_O_RDONLY, &file, priv), 0);
    memset(buf, 0, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
    KT_CHECK_INT_EQ(buf[0], 0x22);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    if (t != NULL) {
        int status = -1;
        KT_CHECK_INT_EQ(task_wait(t->pid, &status, 2000), 0);
    }

    teardown();
}

static void test_dirty_sync(void)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t data[PAGECACHE_PAGE_SIZE];
    struct pagecache_stats st;
    k_u32_t writes0;

    ktest_begin("pagecache: dirty page written on sync");

    setup();
    memset(data, 0x33, sizeof(data));

    KT_CHECK_INT_EQ(vfs_open("/d.bin", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, data, sizeof(data)), (int)sizeof(data));

    pagecache_get_stats(&st);
    KT_CHECK(st.dirty > 0);

    writes0 = g_write_calls;
    KT_CHECK_INT_EQ(vfs_fsync(&file), 0);
    KT_CHECK(g_write_calls > writes0);

    pagecache_get_stats(&st);
    /* Dirty pages for this file should be flushed. */
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    teardown();
}

static void test_eviction(void)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    char name[32];
    struct pagecache_stats st;
    struct kmm_stats km0, km1;
    k_u32_t i;
    k_u32_t cap;

    ktest_begin("pagecache: eviction keeps memory bounded");

    setup();
    pagecache_set_capacity(8);
    pagecache_get_stats(&st);
    cap = (k_u32_t)st.capacity;
    KT_CHECK_INT_EQ(cap, 8u);

    kmm_get_stats(&km0);

    /* Create more files than capacity, one page each. */
    for (i = 0; i < cap + 8u; i++) {
        k_u8_t data[PAGECACHE_PAGE_SIZE];
        memset(data, (int)(i & 0xFF), sizeof(data));
        snprintf(name, sizeof(name), "/e%u.bin", (unsigned)i);
        if (vfs_open(name, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv) == 0) {
            vfs_write(&file, data, sizeof(data));
            vfs_fsync(&file);
            vfs_close(&file);
        }
    }

    /* Read them back to force cache population/eviction. */
    for (i = 0; i < cap + 8u; i++) {
        k_u8_t buf[PAGECACHE_PAGE_SIZE];
        snprintf(name, sizeof(name), "/e%u.bin", (unsigned)i);
        if (vfs_open(name, VFS_O_RDONLY, &file, priv) == 0) {
            vfs_read(&file, buf, sizeof(buf));
            vfs_close(&file);
        }
    }

    pagecache_get_stats(&st);
    KT_CHECK(st.pages <= st.capacity);
    KT_CHECK(st.evictions > 0);

    kmm_get_stats(&km1);
    /* Memory should not grow without bound: allow some slack. */
    KT_CHECK(km1.total_bytes <=
             km0.total_bytes + (k_size_t)(cap + 16u) * (PAGECACHE_PAGE_SIZE + 128u));

    teardown();
}

struct fake_file {
    k_u8_t data[8192];
    k_u32_t size;
    k_u32_t inode;
};

static struct fake_file g_fake;
static bool g_fake_fail_read;
static bool g_fake_fail_write;
static k_u32_t g_fake_reads;
static k_u32_t g_fake_writes;

static int fake_open(void *ctx, const char *path, int flags, struct vfs_file *out)
{
    K_UNUSED(ctx);
    K_UNUSED(path);
    K_UNUSED(flags);
    out->inode = g_fake.inode;
    out->size = g_fake.size;
    out->offset = 0;
    return 0;
}

static int fake_readpage(void *ctx, k_u32_t inode, k_u64_t offset, void *buf, k_size_t size)
{
    K_UNUSED(ctx);
    if (inode != g_fake.inode) {
        return KERR_NOENT;
    }
    g_fake_reads++;
    if (g_fake_fail_read) {
        return KERR_IO;
    }
    if (offset >= g_fake.size) {
        return 0;
    }
    if (offset + size > g_fake.size) {
        size = (k_size_t)(g_fake.size - offset);
    }
    memcpy(buf, g_fake.data + offset, size);
    return (int)size;
}

static int fake_writepage(void *ctx, k_u32_t inode, k_u64_t offset, const void *buf, k_size_t size)
{
    K_UNUSED(ctx);
    if (inode != g_fake.inode) {
        return KERR_NOENT;
    }
    g_fake_writes++;
    if (g_fake_fail_write) {
        return KERR_IO;
    }
    if (offset + size > sizeof(g_fake.data)) {
        return KERR_NOSPC;
    }
    memcpy(g_fake.data + offset, buf, size);
    if (offset + size > g_fake.size) {
        g_fake.size = (k_u32_t)(offset + size);
    }
    return (int)size;
}

static int fake_sync(void *ctx, k_u32_t inode)
{
    K_UNUSED(ctx);
    K_UNUSED(inode);
    return 0;
}

static int fake_close(void *ctx, struct vfs_file *file)
{
    K_UNUSED(ctx);
    K_UNUSED(file);
    return 0;
}

static int fake_stat(void *ctx, const char *path, struct vfs_stat *out)
{
    K_UNUSED(ctx);
    K_UNUSED(path);
    memset(out, 0, sizeof(*out));
    out->inode = g_fake.inode;
    out->type = 1;
    out->size = g_fake.size;
    return 0;
}

static const struct vfs_operations g_fake_ops = {
    .open = fake_open,
    .readpage = fake_readpage,
    .writepage = fake_writepage,
    .sync = fake_sync,
    .close = fake_close,
    .stat = fake_stat,
};

static void test_device_error(void)
{
    struct vfs_file file;
    k_u8_t buf[PAGECACHE_PAGE_SIZE];
    struct pagecache_stats st;
    int rc;

    ktest_begin("pagecache: device error does not corrupt cache");

    pagecache_reset();
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.inode = 42;
    g_fake.size = PAGECACHE_PAGE_SIZE;
    memset(g_fake.data, 0xAB, PAGECACHE_PAGE_SIZE);
    g_fake_fail_read = false;
    g_fake_fail_write = false;
    g_fake_reads = 0;
    g_fake_writes = 0;

    KT_CHECK_INT_EQ(vfs_mount("/fake", &g_fake_ops, NULL), 0);

    /* Successful read populates cache. */
    memset(&file, 0, sizeof(file));
    file.fs = NULL;
    file.ops = &g_fake_ops;
    file.inode = g_fake.inode;
    file.size = g_fake.size;
    file.flags = VFS_O_RDONLY;
    memset(buf, 0, sizeof(buf));
    rc = vfs_read(&file, buf, sizeof(buf));
    KT_CHECK_INT_EQ(rc, (int)sizeof(buf));
    KT_CHECK_INT_EQ(buf[0], 0xAB);

    /* Drop cache, then inject read error. */
    pagecache_drop_all();
    g_fake_fail_read = true;
    file.offset = 0;
    memset(buf, 0, sizeof(buf));
    rc = vfs_read(&file, buf, sizeof(buf));
    KT_CHECK(rc < 0);

    /* Cache should not have a valid page with garbage. */
    g_fake_fail_read = false;
    file.offset = 0;
    memset(buf, 0, sizeof(buf));
    rc = vfs_read(&file, buf, sizeof(buf));
    KT_CHECK_INT_EQ(rc, (int)sizeof(buf));
    KT_CHECK_INT_EQ(buf[0], 0xAB);

    /* Writeback error: dirty page stays dirty. */
    pagecache_reset();
    KT_CHECK_INT_EQ(vfs_mount("/fake2", &g_fake_ops, NULL), 0);
    memset(&file, 0, sizeof(file));
    file.fs = NULL;
    file.ops = &g_fake_ops;
    file.inode = g_fake.inode;
    file.size = g_fake.size;
    file.flags = VFS_O_RDWR;
    file.offset = 0;
    memset(buf, 0xCD, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_write(&file, buf, sizeof(buf)), (int)sizeof(buf));
    pagecache_get_stats(&st);
    KT_CHECK(st.dirty > 0);
    g_fake_fail_write = true;
    rc = vfs_fsync(&file);
    KT_CHECK(rc < 0);
    pagecache_get_stats(&st);
    KT_CHECK(st.dirty > 0);
    g_fake_fail_write = false;
    KT_CHECK_INT_EQ(vfs_fsync(&file), 0);

    vfs_umount("/fake2");
    vfs_umount("/fake");
    pagecache_reset();
}

static void test_boundaries(void)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t data[5000];
    k_u8_t buf[5000];
    int i;
    int rc;

    ktest_begin("pagecache: partial page and boundaries");

    setup();

    for (i = 0; i < (int)sizeof(data); i++) {
        data[i] = (k_u8_t)(i & 0xFF);
    }

    rc = vfs_open("/b.bin", VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, priv);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK_INT_EQ(vfs_write(&file, data, sizeof(data)), (int)sizeof(data));
    KT_CHECK_INT_EQ(vfs_fsync(&file), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    pagecache_drop_all();

    rc = vfs_open("/b.bin", VFS_O_RDONLY, &file, priv);
    KT_CHECK_INT_EQ(rc, 0);
    memset(buf, 0, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
    KT_CHECK(memcmp(buf, data, sizeof(data)) == 0);
    /* EOF */
    KT_CHECK_INT_EQ(vfs_read(&file, buf, 16), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* Partial write in middle. */
    rc = vfs_open("/b.bin", VFS_O_RDWR, &file, priv);
    KT_CHECK_INT_EQ(rc, 0);
    file.offset = 100;
    {
        k_u8_t patch[32];
        memset(patch, 0xEE, sizeof(patch));
        KT_CHECK_INT_EQ(vfs_write(&file, patch, sizeof(patch)), (int)sizeof(patch));
        memcpy(data + 100, patch, sizeof(patch));
    }
    KT_CHECK_INT_EQ(vfs_fsync(&file), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    pagecache_drop_all();
    rc = vfs_open("/b.bin", VFS_O_RDONLY, &file, priv);
    KT_CHECK_INT_EQ(rc, 0);
    memset(buf, 0, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_read(&file, buf, sizeof(buf)), (int)sizeof(buf));
    KT_CHECK(memcmp(buf, data, sizeof(data)) == 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    teardown();
}

void test_pagecache(void)
{
    test_miss_hit();
    test_shared();
    test_dirty_sync();
    test_eviction();
    test_device_error();
    test_boundaries();
}
