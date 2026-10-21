#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"

/* The size of the volume of the tests: 256 blocks of 1 KB. */
#define TEST_BLOCKS 256

static struct mfs_ram_device g_volume;
static struct mfs_device g_device;

static int setup(void)
{
    int rc = mfs_ram_init(&g_volume, TEST_BLOCKS);

    if (rc != 0) {
        return rc;
    }

    mfs_ram_device(&g_volume, &g_device);

    return mfs_format(&g_device, 64);
}

static void teardown(void)
{
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static void test_format(void)
{
    struct mfs_info info;

    ktest_begin("mfs: the volume is formatted and mounted");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_is_mounted());

    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK_INT_EQ(info.total_blocks, TEST_BLOCKS);
    KT_CHECK(info.data_blocks > 0);
    KT_CHECK(info.meta_blocks > 0);
    KT_CHECK(info.inodes_total > 0);
    KT_CHECK_INT_EQ(info.inodes_used, 1); /* only the root */
    KT_CHECK(info.data_free > 0);

    teardown();
}

static void test_layout(void)
{
    struct mfs_info info;
    const k_u8_t *info_block;

    ktest_begin("mfs: the layout follows the design of the documentation");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);

    /* The INFO section is the last block of the volume. */
    info_block = g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE;
    {
        struct mfs_superblock sb;

        memcpy(&sb, info_block, sizeof(sb));

        KT_CHECK_INT_EQ(sb.magic, MFS_MAGIC);
        KT_CHECK_INT_EQ(sb.version, MFS_VERSION);
        KT_CHECK_INT_EQ(sb.block_size, MFS_BLOCK_SIZE);
        KT_CHECK_INT_EQ(sb.total_blocks, TEST_BLOCKS);
        KT_CHECK_INT_EQ(sb.data_start, 0);
        KT_CHECK_INT_EQ(sb.data_blocks, info.data_blocks);
        KT_CHECK_INT_EQ(sb.meta_start, TEST_BLOCKS - info.meta_blocks - 1);
        KT_CHECK_INT_EQ(sb.info_start, TEST_BLOCKS - 1);
        KT_CHECK_INT_EQ(sb.root_inode, MFS_ROOT_INODE);
    }

    teardown();
}

static void test_files(void)
{
    struct mfs_file file;
    struct mfs_inode inode;
    char buffer[128];
    int rc;

    ktest_begin("mfs: a file can be written and read");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/hello.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/hello.txt"), MFS_EEXIST);

    KT_CHECK_INT_EQ(mfs_open("/hello.txt", MFS_O_RDWR, &file), MFS_OK);

    rc = mfs_write(&file, "hello microfs", 13);
    KT_CHECK_INT_EQ(rc, 13);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_open("/hello.txt", MFS_O_RDONLY, &file), MFS_OK);
    memset(buffer, 0, sizeof(buffer));
    rc = mfs_read(&file, buffer, sizeof(buffer));
    KT_CHECK_INT_EQ(rc, 13);
    KT_CHECK_STR_EQ(buffer, "hello microfs");
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_stat("/hello.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 13);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_USED, MFS_INODE_USED);

    teardown();
}

static void test_big_file(void)
{
    struct mfs_file file;
    static char content[5000];
    static char check[5000];
    int rc;
    k_size_t i;

    ktest_begin("mfs: a file that is bigger than a block");

    for (i = 0; i < sizeof(content); i++) {
        content[i] = (char)(i & 0xFF);
    }

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/big.bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDWR, &file), MFS_OK);

    rc = mfs_write(&file, content, sizeof(content));
    KT_CHECK_INT_EQ(rc, (int)sizeof(content));
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDONLY, &file), MFS_OK);
    memset(check, 0, sizeof(check));
    rc = mfs_read(&file, check, sizeof(check));
    KT_CHECK_INT_EQ(rc, (int)sizeof(check));
    KT_CHECK_INT_EQ(memcmp(content, check, sizeof(content)), 0);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    teardown();
}

static void test_directories(void)
{
    struct mfs_dirent entries[16];
    struct mfs_dirent entry;

    ktest_begin("mfs: the directories hold the entries");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_mkdir("/etc"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mkdir("/etc"), MFS_EEXIST);
    KT_CHECK_INT_EQ(mfs_create("/etc/config.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/readme"), MFS_OK);

    /* The root holds two entries. */
    KT_CHECK_INT_EQ(mfs_list("/", entries, 16), MFS_OK);
    KT_CHECK(mfs_stat("/etc", NULL, &entry) == MFS_OK);
    KT_CHECK_STR_EQ(entry.name, "etc");
    KT_CHECK_INT_EQ(entry.type & MFS_INODE_DIR, MFS_INODE_DIR);

    /* The directory holds the file. */
    KT_CHECK_INT_EQ(mfs_list("/etc", entries, 16), MFS_OK);
    KT_CHECK_STR_EQ(entries[0].name, "config.txt");
    KT_CHECK_INT_EQ(entries[0].type & MFS_INODE_DIR, 0);

    teardown();
}

static void test_unlink(void)
{
    struct mfs_info info;
    struct mfs_inode inode;

    ktest_begin("mfs: a file can be removed");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/temp.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK_INT_EQ(info.inodes_used, 2);

    KT_CHECK_INT_EQ(mfs_unlink("/temp.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/temp.txt", &inode, NULL), MFS_ENOENT);
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK_INT_EQ(info.inodes_used, 1);

    /* The directory has to be empty before it can be removed. */
    KT_CHECK_INT_EQ(mfs_mkdir("/dir"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/dir/file"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_unlink("/dir"), MFS_ENOTEMPTY);
    KT_CHECK_INT_EQ(mfs_unlink("/dir/file"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_unlink("/dir"), MFS_OK);

    teardown();
}

static void test_persistence(void)
{
    struct mfs_file file;
    char buffer[32];

    ktest_begin("mfs: the data survives a remount");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/keep.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/keep.txt", MFS_O_RDWR, &file), MFS_OK);
    mfs_write(&file, "persisted", 9);
    mfs_close(&file);

    /* Unmount and mount again: the volume is the same memory. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_open("/keep.txt", MFS_O_RDONLY, &file), MFS_OK);
    memset(buffer, 0, sizeof(buffer));
    mfs_read(&file, buffer, sizeof(buffer));
    KT_CHECK_STR_EQ(buffer, "persisted");
    mfs_close(&file);

    teardown();
}

void test_mfs(void)
{
    test_format();
    test_layout();
    test_files();
    test_big_file();
    test_directories();
    test_unlink();
    test_persistence();
}
