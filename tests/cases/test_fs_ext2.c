/*
 * Micro OS — the ext2 reader next to MicroFS.
 *
 * The image is built by the test itself, byte by byte: a superblock, one
 * group descriptor, a table of inodes, a few directories and the blocks of
 * a file. What the image contains is known exactly, so a reader that finds
 * a name, a size, a moment or a byte is answering about the bytes the test
 * wrote and not about mke2fs of the machine that runs the tests.
 *
 * The image exercises the whole path of a file: the direct blocks, one
 * table of pointers, two and three, and a hole between them. The damage
 * the reader has to survive - a superblock that is not one, an inode
 * outside the table, a block outside the volume - is a write of a byte or
 * of a word into the same image.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "fs_ext2/ext2.h"
#include "kernel/blockdev.h"
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"
#include "port_host.h"

#define TEST_DEV_SECTOR 1024u
#define TEST_ROOT_POINT "/mnt/ext2"
#define TEST_DEVICE_NAME "ext20"

#define TEST_BLOCK_SIZE 1024u
#define TEST_BLOCKS 256u
#define TEST_INODES 32u
#define TEST_INODE_SIZE 128u
#define TEST_INODE_TABLE 5u

#define TEST_FREE_BLOCKS 230u
#define TEST_FREE_INODES 25u

/* The first logical block of the double and the triple indirection. */
#define TEST_DIRECT_BLOCKS 12u
#define TEST_DOUBLE_BLOCK (TEST_DIRECT_BLOCKS + TEST_BLOCK_SIZE / 4u)
#define TEST_TRIPLE_BLOCK (TEST_DOUBLE_BLOCK + (TEST_BLOCK_SIZE / 4u) * (TEST_BLOCK_SIZE / 4u))

/* The inode numbers of the image. */
#define TEST_ROOT_INODE 2u
#define TEST_HELLO_INODE 11u
#define TEST_SUB_INODE 12u
#define TEST_INNER_INODE 13u
#define TEST_LONG_INODE 14u
#define TEST_INDIRECT_INODE 15u
#define TEST_BAD_INODE 16u
#define TEST_OUTSIDE_INODE 500u

/* The blocks of the image. */
#define TEST_ROOT_DIR_BLOCK 9u
#define TEST_SUB_DIR_BLOCK 10u
#define TEST_HELLO_BLOCK 11u
#define TEST_LONG_BLOCK 12u
#define TEST_INDIRECT_BLOCK 13u
#define TEST_SINGLE_TABLE 14u
#define TEST_DOUBLE_TOP 15u
#define TEST_DOUBLE_TABLE 16u
#define TEST_DOUBLE_DATA 17u
#define TEST_TRIPLE_TOP 18u
#define TEST_TRIPLE_MIDDLE 19u
#define TEST_TRIPLE_TABLE 20u
#define TEST_TRIPLE_DATA 21u
#define TEST_SINGLE_DATA 22u
#define TEST_BAD_TOP 23u
#define TEST_BAD_TABLE 24u
#define TEST_INNER_BLOCK 25u

#define TEST_ROOT_MTIME 1500000000u
#define TEST_ROOT_CTIME 1499999999u
#define TEST_HELLO_MTIME 1600000200u
#define TEST_HELLO_CTIME 1600000100u
#define TEST_HELLO_SIZE 11u
#define TEST_LONG_NAME_LEN 104u

struct ext2_image {
    k_u8_t *data;
};

struct test_device {
    k_u8_t *data;
    k_u32_t sectors;
};

static struct mfs_ram_device g_mfs_volume;
static struct k_blockdev g_mfs_device;
static struct ext2_image g_image;
static struct test_device g_ext2_device;
static struct k_blockdev g_ext2_blockdev;
static k_u8_t g_file_data[VFS_FILE_DATA_SIZE];
static k_u32_t g_sync_count;
static char g_long_name[TEST_LONG_NAME_LEN + 1];
static char g_long_path[VFS_PATH_MAX];

static void put16(k_u8_t *p, k_u16_t value)
{
    p[0] = (k_u8_t)(value & 0xFFu);
    p[1] = (k_u8_t)((value >> 8) & 0xFFu);
}

static void put32(k_u8_t *p, k_u32_t value)
{
    p[0] = (k_u8_t)(value & 0xFFu);
    p[1] = (k_u8_t)((value >> 8) & 0xFFu);
    p[2] = (k_u8_t)((value >> 16) & 0xFFu);
    p[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

static const char *long_name(void)
{
    if (g_long_name[0] == '\0') {
        memset(g_long_name, 'a', (size_t)TEST_LONG_NAME_LEN - 4u);
        memcpy(g_long_name + TEST_LONG_NAME_LEN - 4u, ".txt", 5);
    }

    return g_long_name;
}

/* The whole path of the long name: the mount point and the name of 104
 * bytes still fit in the window of a path. */
static const char *long_path(void)
{
    if (g_long_path[0] == '\0') {
        (void)snprintf(g_long_path, sizeof(g_long_path), "%s/%s", TEST_ROOT_POINT, long_name());
    }

    return g_long_path;
}

static k_u8_t *inode_at(k_u32_t number)
{
    return g_image.data + (k_u64_t)TEST_INODE_TABLE * TEST_BLOCK_SIZE +
           (k_u64_t)(number - 1u) * TEST_INODE_SIZE;
}

static k_u8_t *block_at(k_u32_t block)
{
    return g_image.data + (k_u64_t)block * TEST_BLOCK_SIZE;
}

static void image_superblock(void)
{
    k_u8_t *s = g_image.data + 1024u;

    put32(s + 0, TEST_INODES);
    put32(s + 4, TEST_BLOCKS);
    put32(s + 8, 0);
    put32(s + 12, TEST_FREE_BLOCKS);
    put32(s + 16, TEST_FREE_INODES);
    put32(s + 20, 1); /* the first data block of a volume of 1 KiB blocks */
    put32(s + 24, 0); /* 1024 << 0 */
    put32(s + 28, 0);
    put32(s + 32, TEST_BLOCKS); /* blocks per group */
    put32(s + 36, TEST_BLOCKS);
    put32(s + 40, TEST_INODES); /* inodes per group */
    put32(s + 44, 0);
    put32(s + 48, 0);
    put16(s + 52, 0);
    put16(s + 54, 0xFFFFu);

    put16(s + 56, 0xEF53u); /* s_magic */
    put16(s + 58, 1);       /* clean */
    put16(s + 60, 1);       /* continue on error */
    put16(s + 62, 0);

    put32(s + 64, 0);
    put32(s + 68, 0);
    put32(s + 72, 0);
    put32(s + 76, 1); /* the dynamic revision: the inode size is read */
    put16(s + 80, 0);
    put16(s + 82, 0);
    put32(s + 84, 11); /* the first inode that is not reserved */
    put16(s + 88, TEST_INODE_SIZE);
    put16(s + 90, 0);
    put32(s + 92, 0);       /* compat */
    put32(s + 96, 0x0002u); /* incompat: the file type of an entry */
    put32(s + 100, 0);      /* ro compat */
    memcpy(s + 120, "microos-ext2", 12);
    memcpy(s + 136, TEST_ROOT_POINT, strlen(TEST_ROOT_POINT));
}

static void image_group(void)
{
    k_u8_t *g = block_at(2);

    put32(g + 0, 3); /* block bitmap */
    put32(g + 4, 4); /* inode bitmap */
    put32(g + 8, TEST_INODE_TABLE);
    put16(g + 12, TEST_FREE_BLOCKS);
    put16(g + 14, TEST_FREE_INODES);
    put16(g + 16, 2); /* the root and the subdirectory */
}

static void image_inode(k_u32_t number, k_u16_t mode, k_u16_t uid, k_u32_t size, k_u32_t mtime,
                        k_u32_t ctime, k_u16_t links, const k_u32_t *blocks, k_size_t count)
{
    k_u8_t *p = inode_at(number);
    k_size_t i;

    memset(p, 0, TEST_INODE_SIZE);
    put16(p + 0, mode);
    put16(p + 2, uid);
    put32(p + 4, size);
    put32(p + 8, mtime); /* atime, which the VFS does not report */
    put32(p + 12, ctime);
    put32(p + 16, mtime);
    put16(p + 24, 0);
    put16(p + 26, links);
    put32(p + 28, 0);
    put32(p + 32, 0);

    for (i = 0; i < count && i < 15u; i++) {
        put32(p + 40u + i * 4u, blocks[i]);
    }
}

static void image_dir_entry(k_u32_t block, k_u32_t offset, k_u32_t inode, const char *name,
                            k_u8_t type, k_u16_t rec_len)
{
    k_u8_t *p = block_at(block) + offset;
    k_size_t len = strlen(name);

    put32(p + 0, inode);
    put16(p + 4, rec_len);
    p[6] = (k_u8_t)len;
    p[7] = type;
    memcpy(p + 8, name, len);
}

static void image_content(void)
{
    static const k_u32_t root_blocks[] = {TEST_ROOT_DIR_BLOCK};
    static const k_u32_t sub_blocks[] = {TEST_SUB_DIR_BLOCK};
    static const k_u32_t hello_blocks[] = {TEST_HELLO_BLOCK};
    static const k_u32_t inner_blocks[] = {TEST_INNER_BLOCK};
    static const k_u32_t long_blocks[] = {TEST_LONG_BLOCK};
    static const k_u32_t indirect_blocks[15] = {
        TEST_INDIRECT_BLOCK, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, TEST_SINGLE_TABLE, TEST_DOUBLE_TOP,
        TEST_TRIPLE_TOP,
    };
    static const k_u32_t bad_blocks[15] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, TEST_BAD_TOP, 0,
    };

    image_inode(TEST_ROOT_INODE, 0040755, 0, TEST_BLOCK_SIZE, TEST_ROOT_MTIME, TEST_ROOT_CTIME, 3,
                root_blocks, K_ARRAY_SIZE(root_blocks));
    image_inode(TEST_SUB_INODE, 0040755, 0, TEST_BLOCK_SIZE, TEST_ROOT_MTIME, TEST_ROOT_CTIME, 2,
                sub_blocks, K_ARRAY_SIZE(sub_blocks));
    image_inode(TEST_HELLO_INODE, 0100644, 1000, TEST_HELLO_SIZE, TEST_HELLO_MTIME,
                TEST_HELLO_CTIME, 1, hello_blocks, K_ARRAY_SIZE(hello_blocks));
    image_inode(TEST_INNER_INODE, 0100644, 1000, 6, TEST_HELLO_MTIME, TEST_HELLO_CTIME, 1,
                inner_blocks, K_ARRAY_SIZE(inner_blocks));
    image_inode(TEST_LONG_INODE, 0100644, 0, 10, TEST_HELLO_MTIME, TEST_HELLO_CTIME, 1, long_blocks,
                K_ARRAY_SIZE(long_blocks));
    image_inode(TEST_INDIRECT_INODE, 0100644, 0,
                (k_u32_t)(TEST_TRIPLE_BLOCK + 1u) * TEST_BLOCK_SIZE, 0, 0, 1, indirect_blocks,
                K_ARRAY_SIZE(indirect_blocks));
    image_inode(TEST_BAD_INODE, 0100644, 0, (k_u32_t)(TEST_DOUBLE_BLOCK + 1u) * TEST_BLOCK_SIZE, 0,
                0, 1, bad_blocks, K_ARRAY_SIZE(bad_blocks));

    /* The root directory, whose last entry fills the block. */
    image_dir_entry(TEST_ROOT_DIR_BLOCK, 0, TEST_ROOT_INODE, ".", 2, 12);
    image_dir_entry(TEST_ROOT_DIR_BLOCK, 12, TEST_ROOT_INODE, "..", 2, 12);
    image_dir_entry(TEST_ROOT_DIR_BLOCK, 24, TEST_HELLO_INODE, "hello.txt", 1, 20);
    image_dir_entry(TEST_ROOT_DIR_BLOCK, 44, TEST_SUB_INODE, "sub", 2, 12);
    image_dir_entry(TEST_ROOT_DIR_BLOCK, 56, TEST_LONG_INODE, long_name(), 1, 112);
    image_dir_entry(TEST_ROOT_DIR_BLOCK, 168, TEST_INDIRECT_INODE, "indirect.bin", 1, 20);
    image_dir_entry(TEST_ROOT_DIR_BLOCK, 188, TEST_BAD_INODE, "bad.bin", 1, 16);
    image_dir_entry(TEST_ROOT_DIR_BLOCK, 204, TEST_OUTSIDE_INODE, "outside.txt", 1,
                    (k_u16_t)(TEST_BLOCK_SIZE - 204u));

    image_dir_entry(TEST_SUB_DIR_BLOCK, 0, TEST_SUB_INODE, ".", 2, 12);
    image_dir_entry(TEST_SUB_DIR_BLOCK, 12, TEST_ROOT_INODE, "..", 2, 12);
    image_dir_entry(TEST_SUB_DIR_BLOCK, 24, TEST_INNER_INODE, "inner.txt", 1,
                    (k_u16_t)(TEST_BLOCK_SIZE - 24u));

    memcpy(block_at(TEST_HELLO_BLOCK), "hello ext2\n", TEST_HELLO_SIZE);
    memcpy(block_at(TEST_LONG_BLOCK), "long name\n", 10);
    memcpy(block_at(TEST_INNER_BLOCK), "inner\n", 6);

    /* The three levels of indirection, each one one step below the first
     * logical block it serves. */
    put32(block_at(TEST_SINGLE_TABLE), TEST_SINGLE_DATA);
    put32(block_at(TEST_DOUBLE_TOP), TEST_DOUBLE_TABLE);
    put32(block_at(TEST_DOUBLE_TABLE), TEST_DOUBLE_DATA);
    put32(block_at(TEST_TRIPLE_TOP), TEST_TRIPLE_MIDDLE);
    put32(block_at(TEST_TRIPLE_MIDDLE), TEST_TRIPLE_TABLE);
    put32(block_at(TEST_TRIPLE_TABLE), TEST_TRIPLE_DATA);

    put32(block_at(TEST_BAD_TOP), TEST_BAD_TABLE);
    put32(block_at(TEST_BAD_TABLE), TEST_BLOCKS + 100u); /* outside the volume */

    memset(block_at(TEST_INDIRECT_BLOCK), 0x11, TEST_BLOCK_SIZE);
    memset(block_at(TEST_SINGLE_DATA), 0x22, TEST_BLOCK_SIZE);
    memset(block_at(TEST_DOUBLE_DATA), 0x33, TEST_BLOCK_SIZE);
    memset(block_at(TEST_TRIPLE_DATA), 0x44, TEST_BLOCK_SIZE);
}

static void image_free(void)
{
    free(g_image.data);
    memset(&g_image, 0, sizeof(g_image));
}

static void build_image(void)
{
    image_free();

    g_image.data = (k_u8_t *)calloc((size_t)TEST_BLOCKS * TEST_BLOCK_SIZE, 1);
    if (g_image.data == NULL) {
        g_ext2_device.data = NULL;
        g_ext2_device.sectors = 0;
        return;
    }

    image_superblock();
    image_group();
    image_content();

    g_ext2_device.data = g_image.data;
    g_ext2_device.sectors = TEST_BLOCKS * TEST_BLOCK_SIZE / TEST_DEV_SECTOR;
    g_ext2_blockdev.sectors = g_ext2_device.sectors;
}

static int device_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    struct test_device *dev = (struct test_device *)ctx;
    size_t offset = (size_t)sector * TEST_DEV_SECTOR;
    size_t size = (size_t)count * TEST_DEV_SECTOR;

    if (dev->data == NULL || sector >= dev->sectors || count > dev->sectors - sector) {
        return -1;
    }

    memcpy(buf, dev->data + offset, size);
    return 0;
}

static int device_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    struct test_device *dev = (struct test_device *)ctx;
    size_t offset = (size_t)sector * TEST_DEV_SECTOR;
    size_t size = (size_t)count * TEST_DEV_SECTOR;

    if (dev->data == NULL || sector >= dev->sectors || count > dev->sectors - sector) {
        return -1;
    }

    memcpy(dev->data + offset, buf, size);
    return 0;
}

static int device_sync(void *ctx)
{
    K_UNUSED(ctx);

    g_sync_count++;
    return 0;
}

static const struct k_blockdev_ops g_test_ops = {
    .read = device_read,
    .write = device_write,
    .erase = NULL,
    .sync = device_sync,
};

static void setup(void)
{
    if (mfs_ram_init(&g_mfs_volume, 128) != 0) {
        return;
    }

    mfs_ram_device(&g_mfs_volume, &g_mfs_device);
    mfs_format(&g_mfs_device, 32);
    mfs_mount(&g_mfs_device);
    mfs_vfs_register("/");

    memset(&g_ext2_device, 0, sizeof(g_ext2_device));
    g_ext2_blockdev.name = TEST_DEVICE_NAME;
    g_ext2_blockdev.sector_size = TEST_DEV_SECTOR;
    g_ext2_blockdev.sectors = 1;
    g_ext2_blockdev.ops = &g_test_ops;
    g_ext2_blockdev.ctx = &g_ext2_device;
    k_blockdev_register(&g_ext2_blockdev);

    ext2_vfs_register_type();
}

static void teardown(void)
{
    (void)vfs_umount(TEST_ROOT_POINT);
    (void)vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_mfs_volume);
    k_blockdev_unregister(&g_ext2_blockdev);
    image_free();
    memset(&g_ext2_device, 0, sizeof(g_ext2_device));
    vfs_unregister_type("ext2");
}

static int mount_image(void)
{
    int rc;

    build_image();
    if (g_image.data == NULL) {
        return KERR_INVAL;
    }

    rc = vfs_mkdir("/mnt");
    if (rc != 0 && rc != KERR_EXIST) {
        return rc;
    }

    return vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME);
}

static int read_file(const char *path, char *buf, k_size_t size)
{
    struct vfs_file file;
    int rc;

    rc = vfs_open(path, VFS_O_RDONLY, &file, g_file_data);
    if (rc != 0) {
        return rc;
    }

    rc = vfs_read(&file, buf, size);
    vfs_close(&file);

    return rc;
}

/* The bytes of the image, so that a reader that wrote would be seen. */
static k_u64_t image_sum(void)
{
    size_t bytes = (size_t)TEST_BLOCKS * TEST_BLOCK_SIZE;
    k_u64_t sum = 0;
    size_t i;

    for (i = 0; i < bytes; i++) {
        sum = sum * 131u + g_image.data[i];
    }

    return sum;
}

static void test_mount_and_stat(void)
{
    struct vfs_dirent entries[4];
    struct vfs_stat stat;
    struct vfs_statx info;
    k_size_t count = 0;

    ktest_begin("ext2: a volume is mounted next to MicroFS and its inodes are read");

    setup();
    if (mount_image() != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_mount_count(), 2);
    KT_CHECK_STR_EQ(vfs_mount_name(1), TEST_ROOT_POINT);

    /* The mount point is a directory of the tree even though the volume of
     * the root knows nothing about it. */
    KT_CHECK_INT_EQ(vfs_list("/mnt", entries, 4, &count), 0);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_STR_EQ(entries[0].name, "ext2");
    KT_CHECK_INT_EQ(entries[0].type, 2);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT, &stat), 0);
    KT_CHECK_INT_EQ(stat.inode, TEST_ROOT_INODE);
    KT_CHECK_INT_EQ(stat.type, 2);
    KT_CHECK_INT_EQ(stat.mode, 0755);
    KT_CHECK_INT_EQ(stat.owner, 0);
    KT_CHECK_INT_EQ(stat.size, TEST_BLOCK_SIZE);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/hello.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.inode, TEST_HELLO_INODE);
    KT_CHECK_INT_EQ(stat.type, 1);
    KT_CHECK_INT_EQ(stat.size, TEST_HELLO_SIZE);
    KT_CHECK_INT_EQ(stat.mode, 0644);
    KT_CHECK_INT_EQ(stat.owner, 1000);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/sub", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);
    KT_CHECK_INT_EQ(stat.mode, 0755);

    /* The moments of the change are the ones of the inode. */
    KT_CHECK_INT_EQ(vfs_statx(TEST_ROOT_POINT "/hello.txt", &info), 0);
    KT_CHECK_INT_EQ(info.mtime, TEST_HELLO_MTIME);
    KT_CHECK_INT_EQ(info.ctime, TEST_HELLO_CTIME);
    KT_CHECK_INT_EQ(info.mode, 0644);
    KT_CHECK_INT_EQ(info.owner, 1000);

    KT_CHECK_INT_EQ(vfs_statx(TEST_ROOT_POINT, &info), 0);
    KT_CHECK_INT_EQ(info.mtime, TEST_ROOT_MTIME);
    KT_CHECK_INT_EQ(info.ctime, TEST_ROOT_CTIME);

    /* ext2 is case sensitive: the name of the medium is compared byte for
     * byte. */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/HELLO.TXT", &stat), KERR_NOENT);

    teardown();
}

static void test_read(void)
{
    struct vfs_file file;
    char buffer[64];
    k_u8_t byte;

    ktest_begin("ext2: a file is read through its direct and indirect blocks");

    setup();
    if (mount_image() != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/hello.txt", buffer, sizeof(buffer)),
                    TEST_HELLO_SIZE);
    KT_CHECK_STR_EQ(buffer, "hello ext2\n");

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/sub/inner.txt", buffer, sizeof(buffer)), 6);
    KT_CHECK_STR_EQ(buffer, "inner\n");

    /* A short read leaves the descriptor where it stopped. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/hello.txt", VFS_O_RDONLY, &file, g_file_data), 0);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, 5), 5);
    KT_CHECK_STR_EQ(buffer, "hello");
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), 6);
    KT_CHECK_STR_EQ(buffer, " ext2\n");
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* One file with all the levels of indirection. A hole between them is
     * a run of zeros, not a missing file. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/indirect.bin", VFS_O_RDONLY, &file, g_file_data), 0);

    KT_CHECK_INT_EQ(vfs_read(&file, &byte, 1), 1);
    KT_CHECK_INT_EQ(byte, 0x11);

    file.offset = (k_u64_t)TEST_DIRECT_BLOCKS * TEST_BLOCK_SIZE;
    KT_CHECK_INT_EQ(vfs_read(&file, &byte, 1), 1);
    KT_CHECK_INT_EQ(byte, 0x22);

    file.offset = (k_u64_t)TEST_DOUBLE_BLOCK * TEST_BLOCK_SIZE;
    KT_CHECK_INT_EQ(vfs_read(&file, &byte, 1), 1);
    KT_CHECK_INT_EQ(byte, 0x33);

    file.offset = (k_u64_t)TEST_TRIPLE_BLOCK * TEST_BLOCK_SIZE;
    KT_CHECK_INT_EQ(vfs_read(&file, &byte, 1), 1);
    KT_CHECK_INT_EQ(byte, 0x44);

    file.offset = (k_u64_t)(TEST_DIRECT_BLOCKS + 1u) * TEST_BLOCK_SIZE;
    memset(buffer, 0xFF, 4);
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, 4), 4);
    KT_CHECK_INT_EQ(buffer[0], 0);
    KT_CHECK_INT_EQ(buffer[3], 0);

    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    teardown();
}

static void test_dir_listing(void)
{
    struct vfs_dirent entries[16];
    struct vfs_file file;
    struct vfs_stat stat;
    k_size_t count = 0;
    k_size_t total = 0;
    k_size_t i;
    int saw_hello = 0;
    int saw_sub = 0;
    int saw_long = 0;
    int saw_outside = 0;

    ktest_begin("ext2: the entries of a directory, the long names and the dot entries");

    setup();
    if (mount_image() != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT, entries, 16, &count), 0);
    KT_CHECK_INT_EQ(count, 6); /* the two dot entries are hidden */

    for (i = 0; i < count; i++) {
        KT_CHECK(strlen(entries[i].name) <= VFS_NAME_MAX);
        KT_CHECK(strcmp(entries[i].name, ".") != 0);
        KT_CHECK(strcmp(entries[i].name, "..") != 0);

        if (strcmp(entries[i].name, "hello.txt") == 0) {
            saw_hello = 1;
            KT_CHECK_INT_EQ(entries[i].type, 1);
            KT_CHECK_INT_EQ(entries[i].size, TEST_HELLO_SIZE);
        }
        if (strcmp(entries[i].name, "sub") == 0) {
            saw_sub = 1;
            KT_CHECK_INT_EQ(entries[i].type, 2);
        }
        if (strncmp(entries[i].name, long_name(), VFS_NAME_MAX) == 0) {
            saw_long = 1;
        }
        if (strcmp(entries[i].name, "outside.txt") == 0) {
            /* An entry that names an inode the volume does not hold is
             * still a name; its size is not invented. */
            saw_outside = 1;
            KT_CHECK_INT_EQ(entries[i].size, 0);
        }
    }

    KT_CHECK(saw_hello);
    KT_CHECK(saw_sub);
    KT_CHECK(saw_long);
    KT_CHECK(saw_outside);

    /* A name longer than the window of a dirent is cut in the listing, but
     * a path that names it whole still reaches the file. */
    KT_CHECK_INT_EQ(vfs_stat(long_path(), &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 10);

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT "/sub", entries, 16, &count), 0);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_STR_EQ(entries[0].name, "inner.txt");

    /* The descriptor of a directory keeps its own position. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT, VFS_O_RDONLY | VFS_O_DIRECTORY, &file, g_file_data),
                    0);
    for (;;) {
        KT_CHECK_INT_EQ(vfs_readdir(&file, entries, 2, &count), 0);
        if (count == 0) {
            break;
        }
        total += count;
        if (total > 64) {
            break;
        }
    }
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(total, 6);

    teardown();
}

static void test_errors(void)
{
    struct vfs_file file;
    struct vfs_dirent entries[1];
    struct vfs_stat stat;
    k_size_t count = 0;
    k_u8_t byte;

    ktest_begin("ext2: a missing name, an inode outside the table and a block outside the volume");

    setup();
    if (mount_image() != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/missing.txt", &stat), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/hello.txt/inside", &stat), KERR_NOTDIR);

    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/sub", VFS_O_RDONLY, &file, g_file_data), KERR_ISDIR);
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/hello.txt", VFS_O_RDONLY | VFS_O_DIRECTORY, &file, g_file_data),
        KERR_NOTDIR);

    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/hello.txt", VFS_O_RDONLY, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_readdir(&file, entries, 1, &count), KERR_NOTDIR);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* The directory names an inode the table does not hold. */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/outside.txt", &stat), KERR_IO);
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/outside.txt", VFS_O_RDONLY, &file, g_file_data),
                    KERR_IO);

    /* The table of the double indirection names a block past the volume. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/bad.bin", VFS_O_RDONLY, &file, g_file_data), 0);
    file.offset = (k_u64_t)TEST_DOUBLE_BLOCK * TEST_BLOCK_SIZE;
    KT_CHECK_INT_EQ(vfs_read(&file, &byte, 1), KERR_IO);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* An entry whose name does not fit its record is a damaged directory,
     * not a name that is cut. */
    g_image.data[(k_u64_t)TEST_ROOT_DIR_BLOCK * TEST_BLOCK_SIZE + 24u + 6u] = 200u;
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/hello.txt", &stat), KERR_IO);

    teardown();
}

/*
 * --- the image of mke2fs and the tools of the world ---
 *
 * The write path is believed only against the tools of the format: a
 * volume is made by mke2fs on the host, the kernel mounts it, writes it and
 * leaves it, and e2fsck -fn reads the bytes the kernel left. The reverse -
 * an image of mke2fs is read after the writes of a program of the world -
 * is the same check, because e2fsck and debugfs read the same image the
 * kernel did.
 */
#define HOST_IMAGE_BLOCKS 2048u
#define HOST_BLOCK_SIZE 1024u
#define HOST_DIR_TEMPLATE "/tmp/microos-ext2-XXXXXX"

static int host_command(const char *command)
{
    int status = system(command);

    if (status == -1) {
        return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static bool host_tool(const char *name)
{
    char command[128];

    (void)snprintf(command, sizeof(command), "command -v %s >/dev/null 2>&1", name);

    return host_command(command) == 0;
}

static int host_capture(const char *command, char *out, size_t cap)
{
    FILE *pipe = popen(command, "r");
    size_t used = 0;
    int status;

    if (pipe == NULL) {
        return -1;
    }

    if (cap > 1) {
        used = fread(out, 1, cap - 1, pipe);
    }

    out[used] = '\0';

    status = pclose(pipe);

    if (status == -1) {
        return -1;
    }

    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* Take an image of mke2fs into the memory of the test. */
static bool image_load_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    long size;
    size_t got;

    if (file == NULL) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }

    size = ftell(file);

    if (size <= 0 || (size % (long)TEST_DEV_SECTOR) != 0) {
        fclose(file);
        return false;
    }

    rewind(file);

    image_free();
    g_image.data = (k_u8_t *)malloc((size_t)size);

    if (g_image.data == NULL) {
        fclose(file);
        return false;
    }

    got = fread(g_image.data, 1, (size_t)size, file);
    fclose(file);

    if (got != (size_t)size) {
        image_free();
        return false;
    }

    g_ext2_device.data = g_image.data;
    g_ext2_device.sectors = (k_u32_t)((size_t)size / TEST_DEV_SECTOR);
    g_ext2_blockdev.sectors = g_ext2_device.sectors;

    return true;
}

static bool image_store_file(const char *path)
{
    FILE *file = fopen(path, "wb");
    size_t size = (size_t)g_ext2_device.sectors * TEST_DEV_SECTOR;
    bool ok;

    if (file == NULL) {
        return false;
    }

    ok = fwrite(g_image.data, 1, size, file) == size;
    ok = (fclose(file) == 0) && ok;

    return ok;
}

static bool host_mkfs(char *dir, char *image, size_t cap, k_u32_t blocks, k_u32_t block_size,
                      k_u32_t group_blocks, k_u32_t inodes)
{
    char command[512];

    (void)snprintf(dir, cap, "%s", HOST_DIR_TEMPLATE);

    if (mkdtemp(dir) == NULL) {
        return false;
    }

    (void)snprintf(image, cap, "%s/fs.img", dir);

    (void)snprintf(command, sizeof(command),
                   "mke2fs -q -F -t ext2 -b %u -I 256 -g %u -N %u '%s' %u", (unsigned)block_size,
                   (unsigned)group_blocks, (unsigned)inodes, image, (unsigned)blocks);

    return host_command(command) == 0;
}

static int mount_host_image(const char *image)
{
    int rc;

    if (!image_load_file(image)) {
        return KERR_INVAL;
    }

    g_ext2_device.data = g_image.data;
    g_ext2_blockdev.sectors = g_ext2_device.sectors;

    rc = vfs_mkdir("/mnt");
    if (rc != 0 && rc != KERR_EXIST) {
        return rc;
    }

    return vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME);
}

/* e2fsck reads what the kernel left: a clean answer or a line to show. */
static void check_e2fsck(const char *image)
{
    char command[512];
    char output[4096];
    int rc;

    (void)snprintf(command, sizeof(command), "e2fsck -fn '%s' 2>&1", image);
    rc = host_capture(command, output, sizeof(output));

    KT_CHECK_INT_EQ(rc, 0);

    if (rc != 0) {
        ktest_fail(__FILE__, __LINE__, "e2fsck on %s says: %s", image, output);
    }
}

/* The name is in the directory of the image, as the tool of the world sees
 * it. */
static void check_debugfs(const char *image, const char *request, const char *needle)
{
    char command[512];
    char output[8192];
    int rc;

    (void)snprintf(command, sizeof(command), "debugfs -R '%s' '%s' 2>/dev/null", request, image);
    rc = host_capture(command, output, sizeof(output));

    KT_CHECK_INT_EQ(rc, 0);

    if (rc != 0) {
        ktest_fail(__FILE__, __LINE__, "debugfs '%s' on %s says: %s", request, image, output);
        return;
    }

    if (strstr(output, needle) == NULL) {
        ktest_fail(__FILE__, __LINE__, "debugfs '%s' on %s has no '%s': %s", request, image, needle,
                   output);
    }
}

static bool host_tools_ready(void)
{
    return host_tool("mke2fs") && host_tool("e2fsck") && host_tool("debugfs");
}

static void test_read_is_quiet(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    char buffer[64];
    k_u64_t before;
    const char *path = TEST_ROOT_POINT "/hello.txt";

    ktest_begin("ext2: a volume that is only read is left exactly as it was");

    setup();
    if (mount_image() != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    before = image_sum();
    g_sync_count = 0;

    KT_CHECK_INT_EQ(vfs_open(path, VFS_O_RDONLY, &file, g_file_data), 0);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), TEST_HELLO_SIZE);
    KT_CHECK_STR_EQ(buffer, "hello ext2\n");
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/sub", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);

    /* Nothing reached the medium and the device was not asked to sync. */
    KT_CHECK_INT_EQ(g_sync_count, 0);
    KT_CHECK_INT_EQ(image_sum(), before);

    teardown();
}

static void test_write_create_read(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    char image[256];
    char dir[256];
    char *pattern;
    char *readback;
    const k_size_t size = 20000u; /* twelve direct blocks and then the table */
    int rc;

    ktest_begin(
        "ext2: a file is created, written through the direct blocks and the table, and read back");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), HOST_IMAGE_BLOCKS, HOST_BLOCK_SIZE, 1024u, 512u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    pattern = (char *)malloc(size + 4u);
    readback = (char *)malloc(size + 4u);
    KT_CHECK(pattern != NULL && readback != NULL);

    if (pattern == NULL || readback == NULL) {
        free(pattern);
        free(readback);
        (void)unlink(image);
        (void)rmdir(dir);
        teardown();
        return;
    }

    for (k_size_t i = 0; i < size; i++) {
        pattern[i] = (char)('a' + (char)(i % 26u));
    }
    memcpy(pattern + size, "END\n", 4);

    KT_CHECK_INT_EQ(mount_host_image(image), 0);

    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/big.txt", VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, pattern, size), (int)size);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/big.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, size);

    memset(readback, 0, size + 4u);
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/big.txt", readback, size), (int)size);
    KT_CHECK(memcmp(readback, pattern, size) == 0);

    /* The descriptor that appends writes at the end and moves the size. */
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/big.txt", VFS_O_WRONLY | VFS_O_APPEND, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, pattern + size, 4), 4);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/big.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, size + 4);

    /* A directory, its child, and a file of one byte. */
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/dir"), 0);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/dir/sub"), 0);

    rc = vfs_open(TEST_ROOT_POINT "/dir/sub/note.txt", VFS_O_WRONLY | VFS_O_CREAT, &file,
                  g_file_data);
    KT_CHECK_INT_EQ(rc, 0);

    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_write(&file, "hello\n", 6), 6);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/dir", &stat), 0);
    KT_CHECK_INT_EQ(stat.type, 2);
    KT_CHECK_INT_EQ(stat.mode, 0755);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/dir/sub/note.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 6);
    KT_CHECK_INT_EQ(stat.mode, 0644);

    /* A rename is a call this filesystem does not have: the VFS answers
     * with the word of an operation a volume does not offer. */
    KT_CHECK_INT_EQ(vfs_rename(TEST_ROOT_POINT "/big.txt", TEST_ROOT_POINT "/renamed.txt"),
                    KERR_NOTSUP);

    /* A second name of the same parent is a new file, and a name that is
     * there is not made again. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/dir/sub/note.txt", VFS_O_WRONLY | VFS_O_CREAT, &file,
                             g_file_data),
                    0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));

    check_e2fsck(image);
    check_debugfs(image, "stat /big.txt", "Size: 20004");
    check_debugfs(image, "stat /dir/sub/note.txt", "Size: 6");
    check_debugfs(image, "ls -l /dir/sub", "note.txt");

    /* The same image is mounted again: the bytes of the file come back from
     * the medium, not from the memory of the writer, and the check of the
     * machine agrees with e2fsck. */
    KT_CHECK_INT_EQ(mount_host_image(image), 0);

    {
        struct ext2_check_report report;

        KT_CHECK_INT_EQ(ext2_check(TEST_ROOT_POINT, &report), 0);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK_INT_EQ(report.files, 2);       /* big.txt and note.txt */
        KT_CHECK_INT_EQ(report.directories, 4); /* root, lost+found, dir and sub */
    }

    memset(readback, 0, size + 4u);
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/big.txt", readback, size + 4), (int)size + 4);
    KT_CHECK(memcmp(readback, pattern, size + 4) == 0);
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);

    free(pattern);
    free(readback);
    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_many_files(void)
{
    static struct vfs_dirent entries[220];
    char image[256];
    char dir[256];
    char path[VFS_PATH_MAX];
    char content[32];
    struct vfs_file file;
    struct vfs_stat stat;
    k_size_t count = 0;
    int made = 0;

    ktest_begin("ext2: a directory grows past its first block and stays one chain");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), HOST_IMAGE_BLOCKS, HOST_BLOCK_SIZE, 1024u, 512u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/many"), 0);

    for (unsigned i = 0; i < 200u; i++) {
        int used = snprintf(content, sizeof(content), "file %u\n", i);

        (void)snprintf(path, sizeof(path), TEST_ROOT_POINT "/many/f%03u.txt", i);

        if (vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data) != 0) {
            break;
        }

        if (vfs_write(&file, content, (k_size_t)used) != used) {
            (void)vfs_close(&file);
            break;
        }

        if (vfs_close(&file) != 0) {
            break;
        }

        made++;
    }

    KT_CHECK_INT_EQ(made, 200);

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT "/many", entries, 220, &count), 0);
    KT_CHECK_INT_EQ(count, 200);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/many/f199.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 9); /* "file 199" and a newline */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/many", &stat), 0);
    KT_CHECK(stat.size > HOST_BLOCK_SIZE); /* the directory grew */

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);
    check_debugfs(image, "ls /many", "f199.txt");

    /* Half of the names are gone: the records are merged and the volume is
     * still clean. */
    KT_CHECK_INT_EQ(mount_host_image(image), 0);

    for (unsigned i = 0; i < 200u; i += 2) {
        (void)snprintf(path, sizeof(path), TEST_ROOT_POINT "/many/f%03u.txt", i);
        KT_CHECK_INT_EQ(vfs_unlink(path), 0);
    }

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT "/many", entries, 220, &count), 0);
    KT_CHECK_INT_EQ(count, 100);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/many/f001.txt", &stat), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/many/f000.txt", &stat), KERR_NOENT);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);

    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_truncate_and_unlink(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    struct vfs_statfs before;
    struct vfs_statfs after;
    char image[256];
    char dir[256];
    char buffer[64];
    char *content;
    const k_size_t size = 30000u; /* thirty blocks: the table is used */
    k_size_t i;

    ktest_begin("ext2: a file is cut to a smaller size and to nothing, and a name is removed");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), HOST_IMAGE_BLOCKS, HOST_BLOCK_SIZE, 1024u, 512u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    content = (char *)malloc(size);
    KT_CHECK(content != NULL);

    if (content == NULL) {
        (void)unlink(image);
        (void)rmdir(dir);
        teardown();
        return;
    }

    for (i = 0; i < size; i++) {
        content[i] = (char)('A' + (char)(i % 26u));
    }

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &before), 0);

    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/cut.txt", VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, content, size), (int)size);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK(after.free_blocks < before.free_blocks);

    /* Five kilobytes keep the first five blocks; the table and the rest of
     * the blocks are given back. The cut is made through the descriptor,
     * the operation that ftruncate asks the VFS for. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/cut.txt", VFS_O_RDWR, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_truncate(&file, 5000u), 0);
    KT_CHECK_INT_EQ(file.size, 5000);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* A reader has no cut, and a size beyond the volume is refused. */
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/cut.txt", VFS_O_RDONLY, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_truncate(&file, 100u), KERR_BADF);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/cut.txt", VFS_O_RDWR, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_truncate(&file, (k_u64_t)1u << 40), KERR_NOSPC);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/cut.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 5000);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/cut.txt", buffer, sizeof(buffer)), 64);
    KT_CHECK(memcmp(buffer, content, 64) == 0);

    /* Five blocks of a thousand and thirty went back to the bitmap: the
     * twenty-five data blocks of the tail and the table of pointers. */
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks + 5, before.free_blocks);

    /* Growing again is a hole, and a hole reads as zeros. */
    KT_CHECK_INT_EQ(ext2_vfs_truncate(TEST_ROOT_POINT "/cut.txt", 20000u), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/cut.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 20000);
    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/cut.txt", VFS_O_RDONLY, &file, g_file_data), 0);
    file.offset = 6000u;
    memset(buffer, 0xFF, sizeof(buffer));
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), 64);
    for (i = 0; i < sizeof(buffer); i++) {
        KT_CHECK_INT_EQ(buffer[i], 0);
    }
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* Cut to nothing through the open of the descriptor, the way a program
     * makes a log empty. */
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/cut.txt", VFS_O_RDWR | VFS_O_TRUNC, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/cut.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks, before.free_blocks);

    /* A name is made, written and removed, and the volume comes back to the
     * bytes it had before the name. */
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/gone.txt", VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, content, 100), 100);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/gone.txt"), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/gone.txt", &stat), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks, before.free_blocks);

    /* An object that is gone cannot be removed twice. */
    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/gone.txt"), KERR_NOENT);

    /* The inode of the name that is gone is used again, and no field of
     * the record of the old file comes with it. */
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/again.txt", VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, content, 50), 50);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/again.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 50);
    KT_CHECK_INT_EQ(stat.mode, 0644);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);

    free(content);
    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_no_room(void)
{
    char image[256];
    char dir[256];
    char *content;
    struct vfs_file file;
    struct vfs_stat stat;
    struct vfs_statfs before;
    struct vfs_statfs after;
    const k_size_t too_big = 274433u; /* one byte past twelve direct blocks and one table */
    int rc;

    ktest_begin("ext2: a file that would need a double table is no room, not a broken volume");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), HOST_IMAGE_BLOCKS, HOST_BLOCK_SIZE, 1024u, 512u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    content = (char *)malloc(too_big);
    KT_CHECK(content != NULL);

    if (content == NULL) {
        (void)unlink(image);
        (void)rmdir(dir);
        teardown();
        return;
    }

    memset(content, 'z', too_big);

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &before), 0);

    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/huge.bin", VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
    rc = vfs_write(&file, content, too_big);
    KT_CHECK_INT_EQ(rc, KERR_NOSPC);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    /* What was written before the wall is a file of its own size and the
     * volume is still consistent. */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/huge.bin", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 274432);

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/huge.bin"), 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks, before.free_blocks);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);

    free(content);
    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_attributes(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    struct vfs_statx info;
    char image[256];
    char dir[256];

    ktest_begin("ext2: the mode and the owner change on the medium, and the moment moves");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), HOST_IMAGE_BLOCKS, HOST_BLOCK_SIZE, 1024u, 512u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/attr.txt", VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/attr.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.mode, 0644);
    KT_CHECK_INT_EQ(stat.owner, 0);

    KT_CHECK_INT_EQ(vfs_chmod(TEST_ROOT_POINT "/attr.txt", 0600), 0);
    KT_CHECK_INT_EQ(vfs_chown(TEST_ROOT_POINT "/attr.txt", 1000), 0);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/attr.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.mode, 0600);
    KT_CHECK_INT_EQ(stat.owner, 1000);

    KT_CHECK_INT_EQ(vfs_statx(TEST_ROOT_POINT "/attr.txt", &info), 0);
    KT_CHECK(info.ctime > 0);

    /* A mode with bits beyond the twelve of the format is not a mode. */
    KT_CHECK_INT_EQ(vfs_chmod(TEST_ROOT_POINT "/attr.txt", 010000), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_chown(TEST_ROOT_POINT "/attr.txt", 0x10000u), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_chmod(TEST_ROOT_POINT "/missing.txt", 0600), KERR_NOENT);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);
    check_debugfs(image, "stat /attr.txt", "Mode:  0600");
    check_debugfs(image, "stat /attr.txt", "User:  1000");

    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_statfs(void)
{
    struct vfs_statfs volume;

    ktest_begin("ext2: the superblock answers for the blocks and the inodes");

    setup();
    if (mount_image() != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &volume), 0);
    KT_CHECK_INT_EQ(volume.total_blocks, TEST_BLOCKS);
    KT_CHECK_INT_EQ(volume.free_blocks, TEST_FREE_BLOCKS);
    KT_CHECK_INT_EQ(volume.total_inodes, TEST_INODES);
    KT_CHECK_INT_EQ(volume.free_inodes, TEST_FREE_INODES);
    KT_CHECK_INT_EQ(volume.block_size, TEST_BLOCK_SIZE);

    teardown();
}

static void test_bad_images(void)
{
    k_u8_t *sb;

    ktest_begin("ext2: a superblock that is not one, a feature and a table that cannot be walked");

    setup();

    KT_CHECK(vfs_type_find("ext2") >= 0);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", "no-such-device"), KERR_NOENT);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "no-such-type", TEST_DEVICE_NAME), KERR_INVAL);

    build_image();
    if (g_image.data == NULL) {
        KT_CHECK(0);
        teardown();
        return;
    }

    /* The magic is not the one of an ext2 volume. */
    sb = g_image.data + 1024u;
    sb[56] = 0;
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), KERR_INVAL);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);

    /* A block of 32 KiB is not a block this format has. */
    build_image();
    put32(g_image.data + 1024u + 24u, 5);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), KERR_NOTSUP);

    /* An extent tree is ext4 and not a list of direct pointers. */
    build_image();
    put32(g_image.data + 1024u + 96u, 0x0040u);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), KERR_NOTSUP);

    /* A journal needs a writer that replays it. */
    build_image();
    put32(g_image.data + 1024u + 92u, 0x0004u);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), KERR_NOTSUP);

    /* The revision of the format is 0 or 1, and nothing else. */
    build_image();
    put32(g_image.data + 1024u + 76u, 2);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), KERR_NOTSUP);

    /* The inode table of the first group is not on the medium. */
    build_image();
    put32(g_image.data + 2u * TEST_BLOCK_SIZE + 8u, 0);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), KERR_INVAL);

    /* A dynamic revision with no size of an inode. */
    build_image();
    put16(g_image.data + 1024u + 88u, 0);
    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), KERR_INVAL);

    KT_CHECK_INT_EQ(vfs_mount_count(), 1);

    /* The same image mounts once the damage is gone. */
    KT_CHECK_INT_EQ(mount_image(), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 2);

    teardown();
}

static void test_shell_mount(void)
{
    char buf[2048];
    size_t used;
    FILE *sink;

    ktest_begin("ext2: the shell mounts the volume by its type, lists it and reads a file");

    setup();

    build_image();
    if (g_image.data == NULL) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_mkdir("/mnt"), 0);
    ksys_init();
    KT_CHECK_INT_EQ(kshell_init(), KSHELL_OK);

    sink = tmpfile();
    KT_CHECK(sink != NULL);
    if (sink == NULL) {
        teardown();
        return;
    }

    port_host_set_sink(sink);

    KT_CHECK_INT_EQ(kshell_execute("mount " TEST_DEVICE_NAME " " TEST_ROOT_POINT " ext2"),
                    KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("ls " TEST_ROOT_POINT), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("cat " TEST_ROOT_POINT "/hello.txt"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("stat " TEST_ROOT_POINT "/hello.txt"), KSHELL_OK);
    KT_CHECK_INT_EQ(kshell_execute("umount " TEST_ROOT_POINT), KSHELL_OK);

    rewind(sink);
    used = fread(buf, 1, sizeof(buf) - 1, sink);
    buf[used] = '\0';

    KT_CHECK(strstr(buf, "is mounted at " TEST_ROOT_POINT) != NULL);
    KT_CHECK(strstr(buf, "hello.txt") != NULL);
    KT_CHECK(strstr(buf, "hello ext2") != NULL);
    KT_CHECK(strstr(buf, "owner: 1000") != NULL);
    KT_CHECK(strstr(buf, "mode: 0644") != NULL);

    port_host_set_sink(NULL);

    KT_CHECK_INT_EQ(vfs_mount_count(), 1);

    teardown();
}

static void test_remount(void)
{
    struct vfs_stat stat;
    char buffer[32];

    ktest_begin("ext2: the volume is mounted again after it is unmounted");

    setup();
    if (mount_image() != 0) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/hello.txt", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, TEST_HELLO_SIZE);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 1);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/hello.txt", &stat), KERR_NOENT);

    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), 0);
    KT_CHECK_INT_EQ(vfs_mount_count(), 2);

    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/hello.txt", buffer, sizeof(buffer)),
                    TEST_HELLO_SIZE);
    KT_CHECK_STR_EQ(buffer, "hello ext2\n");

    teardown();
}

static void test_many_groups(void)
{
    static struct vfs_dirent entries[140];
    struct vfs_file file;
    struct vfs_statfs before;
    struct vfs_statfs after;
    char image[256];
    char dir[256];
    char path[VFS_PATH_MAX];
    char *block;
    const k_size_t large = 90u * 1024u; /* ninety blocks: a group of 256 blocks is crossed */
    k_size_t count = 0;

    ktest_begin("ext2: a volume of four groups takes blocks and inodes from each of them");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), 1024u, 1024u, 256u, 512u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    block = (char *)malloc(large);
    KT_CHECK(block != NULL);

    if (block == NULL) {
        (void)unlink(image);
        (void)rmdir(dir);
        teardown();
        return;
    }

    memset(block, 'g', large);

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &before), 0);

    {
        static const char *names[] = {"a.bin", "b.bin", "c.bin"};

        for (k_size_t i = 0; i < 3u; i++) {
            (void)snprintf(path, sizeof(path), TEST_ROOT_POINT "/%s", names[i]);
            KT_CHECK_INT_EQ(vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
            KT_CHECK_INT_EQ(vfs_write(&file, block, large), (int)large);
            KT_CHECK_INT_EQ(vfs_close(&file), 0);
        }
    }

    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/small"), 0);

    for (unsigned i = 0; i < 130u; i++) {
        (void)snprintf(path, sizeof(path), TEST_ROOT_POINT "/small/s%03u", i);

        if (vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data) != 0) {
            KT_CHECK(0);
            break;
        }

        KT_CHECK_INT_EQ(vfs_write(&file, "x", 1), 1);
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT "/small", entries, 140, &count), 0);
    KT_CHECK_INT_EQ(count, 130);

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK(after.free_blocks + 273u + 130u + 2u <= before.free_blocks);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);
    check_debugfs(image, "stat /c.bin", "Size: 92160");
    check_debugfs(image, "stat /small/s129", "Size: 1");

    /* Every block and every inode of the four groups goes back. */
    KT_CHECK_INT_EQ(mount_host_image(image), 0);

    {
        static const char *names[] = {"a.bin", "b.bin", "c.bin"};

        for (k_size_t i = 0; i < 3u; i++) {
            (void)snprintf(path, sizeof(path), TEST_ROOT_POINT "/%s", names[i]);
            KT_CHECK_INT_EQ(vfs_unlink(path), 0);
        }
    }

    for (unsigned i = 0; i < 130u; i++) {
        (void)snprintf(path, sizeof(path), TEST_ROOT_POINT "/small/s%03u", i);
        KT_CHECK_INT_EQ(vfs_unlink(path), 0);
    }

    /* The names are gone; the two blocks of the empty directory that held
     * them are still a directory, and nothing else is left. */
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks + 2u, before.free_blocks);

    /* The empty directory of the last group is still a directory. */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/small", &(struct vfs_stat){0}), 0);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);

    free(block);
    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_large_blocks(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    char image[256];
    char dir[256];
    char path[VFS_PATH_MAX];
    char content[5000];
    char readback[5000];
    k_size_t count = 0;
    static struct vfs_dirent entries[32];

    ktest_begin("ext2: a volume of blocks of 4096 bytes takes a file across two of them");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), 512u, 4096u, 1024u, 256u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    for (k_size_t i = 0; i < sizeof(content); i++) {
        content[i] = (char)('A' + (char)(i % 26u));
    }

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/dir"), 0);

    for (unsigned i = 0; i < 20u; i++) {
        (void)snprintf(path, sizeof(path), TEST_ROOT_POINT "/dir/f%02u", i);
        KT_CHECK_INT_EQ(vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
        KT_CHECK_INT_EQ(vfs_write(&file, content, sizeof(content)), (int)sizeof(content));
        KT_CHECK_INT_EQ(vfs_close(&file), 0);
    }

    KT_CHECK_INT_EQ(vfs_list(TEST_ROOT_POINT "/dir", entries, 32, &count), 0);
    KT_CHECK_INT_EQ(count, 20);

    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/dir/f19", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, sizeof(content));

    memset(readback, 0, sizeof(readback));
    KT_CHECK_INT_EQ(read_file(TEST_ROOT_POINT "/dir/f19", readback, sizeof(readback)),
                    (int)sizeof(readback));
    KT_CHECK(memcmp(readback, content, sizeof(content)) == 0);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);
    check_debugfs(image, "stat /dir/f19", "Size: 5000");

    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_unlink_of_a_big_file(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    struct vfs_statfs before;
    struct vfs_statfs after;
    char image[256];
    char dir[256];
    char blob[512];
    char command[1024];
    char buffer[128];
    const k_size_t size = 400u * 1024u; /* past one table: a double table of ext2 */
    FILE *made;

    ktest_begin(
        "ext2: a file of a double table, made by debugfs, is cut and removed by the kernel");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), HOST_IMAGE_BLOCKS, HOST_BLOCK_SIZE, 1024u, 512u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    (void)snprintf(blob, sizeof(blob), "%s/blob.bin", dir);
    made = fopen(blob, "wb");
    KT_CHECK(made != NULL);

    if (made == NULL) {
        (void)unlink(image);
        (void)rmdir(dir);
        teardown();
        return;
    }

    for (k_size_t i = 0; i < size; i++) {
        (void)fputc((int)('a' + (char)(i % 26u)), made);
    }
    KT_CHECK_INT_EQ(fclose(made), 0);

    (void)snprintf(command, sizeof(command), "debugfs -w -R 'mkdir /big' '%s' >/dev/null 2>&1",
                   image);
    KT_CHECK_INT_EQ(host_command(command), 0);
    (void)snprintf(command, sizeof(command),
                   "debugfs -w -R 'write %s /big/data' '%s' >/dev/null 2>&1", blob, image);
    KT_CHECK_INT_EQ(host_command(command), 0);

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/big/data", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, size);

    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/big/data", VFS_O_RDONLY, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, sizeof(buffer)), (int)sizeof(buffer));
    KT_CHECK_INT_EQ(buffer[0], 'a');
    file.offset = 300u * 1024u;
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, 8), 8);
    KT_CHECK_INT_EQ(buffer[0], 'a' + (char)((300u * 1024u) % 26u));
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &before), 0);

    /* A cut inside the double table keeps the head and gives the rest of
     * the three levels back. */
    KT_CHECK_INT_EQ(ext2_vfs_truncate(TEST_ROOT_POINT "/big/data", 100u * 1024u), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/big/data", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 100u * 1024u);

    /* The file is 400 blocks in twelve direct, one table and a double
     * table. A cut at 100 keeps 12 direct and 88 of the table: 168 blocks
     * of the table, the 132 blocks of the double and its two tables are
     * given back - 302 blocks of the medium. */
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks, before.free_blocks + 302u);

    KT_CHECK_INT_EQ(vfs_open(TEST_ROOT_POINT "/big/data", VFS_O_RDONLY, &file, g_file_data), 0);
    file.offset = 99u * 1024u;
    KT_CHECK_INT_EQ(vfs_read(&file, buffer, 64), 64);
    KT_CHECK_INT_EQ(buffer[0], 'a' + (char)((99u * 1024u) % 26u));
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(vfs_unlink(TEST_ROOT_POINT "/big/data"), 0);
    KT_CHECK_INT_EQ(vfs_statfs(TEST_ROOT_POINT, &after), 0);
    KT_CHECK_INT_EQ(after.free_blocks, before.free_blocks + 403u);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);

    (void)unlink(blob);
    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_check(void)
{
    struct ext2_check_report report;
    struct vfs_file file;
    struct vfs_stat stat;
    char image[256];
    char dir[256];
    char command[512];

    ktest_begin(
        "ext2: the check of the machine sees a sound tree and names a superblock that lies");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), HOST_IMAGE_BLOCKS, HOST_BLOCK_SIZE, 1024u, 512u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/dir"), 0);
    KT_CHECK_INT_EQ(
        vfs_open(TEST_ROOT_POINT "/dir/a.txt", VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data), 0);
    KT_CHECK_INT_EQ(vfs_write(&file, "0123456789", 10), 10);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    KT_CHECK_INT_EQ(ext2_check(TEST_ROOT_POINT, &report), 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.directories, 3); /* the root, lost+found and /dir */
    KT_CHECK_INT_EQ(report.files, 1);
    KT_CHECK_INT_EQ(report.groups, 2);
    KT_CHECK_INT_EQ(report.blocks, HOST_IMAGE_BLOCKS);
    KT_CHECK(report.free_blocks > 0);
    KT_CHECK_STR_EQ(report.first, "");

    /* A point of another filesystem is not the volume of this one. */
    KT_CHECK_INT_EQ(ext2_check("/", &report), KERR_INVAL);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));

    /* The superblock says one block more is free than the bitmaps hold. */
    {
        k_u8_t *p = g_image.data + 1024u + 12u;
        k_u32_t free_blocks =
            (k_u32_t)p[0] | ((k_u32_t)p[1] << 8) | ((k_u32_t)p[2] << 16) | ((k_u32_t)p[3] << 24);

        put32(p, free_blocks + 1u);
    }

    KT_CHECK_INT_EQ(vfs_mount_type(TEST_ROOT_POINT, "ext2", TEST_DEVICE_NAME), 0);
    KT_CHECK_INT_EQ(ext2_check(TEST_ROOT_POINT, &report), 0);
    KT_CHECK(report.errors > 0);
    KT_CHECK(strstr(report.first, "superblock") != NULL);
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);

    /* The root of a directory is a chain of records; a size of zero is not
     * a chain, and the walk says so. */
    (void)snprintf(command, sizeof(command),
                   "debugfs -w -R 'set_inode_field /dir size 0' '%s' >/dev/null 2>&1", image);
    KT_CHECK_INT_EQ(host_command(command), 0);
    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(ext2_check(TEST_ROOT_POINT, &report), 0);
    KT_CHECK(report.errors > 0);
    KT_CHECK(strstr(report.first, "not a whole number") != NULL);

    /* The check is a reader: the volume still holds the size it was given. */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/dir", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 0);
    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);

    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

static void test_directory_past_the_direct_blocks(void)
{
    struct vfs_file file;
    struct vfs_stat stat;
    char image[256];
    char dir[256];
    char path[VFS_PATH_MAX];
    unsigned made = 0;

    ktest_begin("ext2: a directory of 1100 names grows through the table of pointers");

    setup();

    if (!host_tools_ready()) {
        printf("  (skipped: mke2fs, e2fsck and debugfs of e2fsprogs are not installed)\n");
        teardown();
        return;
    }

    if (!host_mkfs(dir, image, sizeof(image), 4096u, 1024u, 1024u, 2048u)) {
        KT_CHECK(0);
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(mount_host_image(image), 0);
    KT_CHECK_INT_EQ(vfs_mkdir(TEST_ROOT_POINT "/wide"), 0);

    for (unsigned i = 0; i < 1100u; i++) {
        (void)snprintf(path, sizeof(path), TEST_ROOT_POINT "/wide/n%04u", i);

        if (vfs_open(path, VFS_O_WRONLY | VFS_O_CREAT, &file, g_file_data) != 0) {
            break;
        }

        if (vfs_close(&file) != 0) {
            break;
        }

        made++;
    }

    KT_CHECK_INT_EQ(made, 1100);

    /* 1100 records of sixteen bytes are eighteen blocks: the last six are
     * reached through the table of pointers. */
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/wide", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 18u * HOST_BLOCK_SIZE);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/wide/n0000", &stat), 0);
    KT_CHECK_INT_EQ(stat.size, 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/wide/n1099", &stat), 0);
    KT_CHECK_INT_EQ(vfs_stat(TEST_ROOT_POINT "/wide/n1100", &stat), KERR_NOENT);

    KT_CHECK_INT_EQ(vfs_umount(TEST_ROOT_POINT), 0);
    KT_CHECK(image_store_file(image));
    check_e2fsck(image);
    check_debugfs(image, "stat /wide/n1099", "Inode: ");

    (void)unlink(image);
    (void)rmdir(dir);

    teardown();
}

void test_fs_ext2(void)
{
    test_mount_and_stat();
    test_read();
    test_dir_listing();
    test_errors();
    test_read_is_quiet();
    test_statfs();
    test_bad_images();
    test_shell_mount();
    test_remount();
    test_write_create_read();
    test_many_files();
    test_truncate_and_unlink();
    test_no_room();
    test_attributes();
    test_many_groups();
    test_large_blocks();
    test_unlink_of_a_big_file();
    test_check();
    test_directory_past_the_direct_blocks();
}
