#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/sched.h"
#include "kernel/task.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

/* The size of the volume of the tests: 256 blocks of 1 KB. */
#define TEST_BLOCKS 256

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

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

/*
 * The address of a record of the table of the inodes in the volume of the
 * test. The layout is computed the way the format computes it, so the
 * test can put an old record into a volume of today.
 */
static k_u8_t *inode_record(k_u32_t index)
{
    struct mfs_superblock sb;
    k_u32_t bitmap_blocks;

    memcpy(&sb, g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE, sizeof(sb));
    bitmap_blocks = (sb.data_blocks + 8u * MFS_BLOCK_SIZE - 1u) / (8u * MFS_BLOCK_SIZE);

    return g_volume.data + (size_t)(sb.meta_start + bitmap_blocks) * MFS_BLOCK_SIZE +
           (size_t)index * MFS_INODE_SIZE;
}

/*
 * Put a record into the table the way a writer of the device would: the
 * test goes through the block device and not through the array in the
 * memory, so the record reaches the volume the way it does on the board.
 */
static int store_inode_record(k_u32_t index, const struct mfs_inode *inode)
{
    k_u8_t *where = inode_record(index);
    k_u8_t block[MFS_BLOCK_SIZE];
    size_t offset = (size_t)(where - g_volume.data);
    k_u32_t sector = (k_u32_t)(offset / MFS_BLOCK_SIZE);

    if (g_device.ops->read(g_device.ctx, sector, block, 1) != 0) {
        return -1;
    }

    memcpy(block + offset % MFS_BLOCK_SIZE, inode, sizeof(*inode));

    return g_device.ops->write(g_device.ctx, sector, block, 1);
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

    /* A fresh volume has nothing to hide: the check finds no problem. */
    {
        struct mfs_check_report report;
        k_u32_t writes = g_volume.writes;

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK_INT_EQ(report.files, 0);
        KT_CHECK_INT_EQ(report.directories, 1); /* only the root */
        KT_CHECK_INT_EQ(report.used_blocks, 1); /* the block of the root */
        KT_CHECK_INT_EQ(report.free_blocks, info.data_free);
        KT_CHECK_INT_EQ(report.free_blocks + report.used_blocks, info.data_blocks);
        KT_CHECK_INT_EQ(report.first[0], '\0');

        /* The check only reads: the medium is written not even once. */
        KT_CHECK_INT_EQ(g_volume.writes, writes);
    }

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

    /* The header of the volume lives in the last block of the volume. */
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
        KT_CHECK_INT_EQ(sb.root_inode, MFS_ROOT_INODE);

        /* The three sections fill the volume, in the documented order. */
        KT_CHECK_INT_EQ(sb.meta_start + sb.meta_blocks, sb.info_start);
        KT_CHECK_INT_EQ(sb.info_start + sb.info_blocks, sb.total_blocks);
        KT_CHECK_INT_EQ(sb.meta_start, TEST_BLOCKS - info.meta_blocks - sb.info_blocks);

        /*
         * The INFO section starts at the border of an erase sector of the
         * flash (4 KB = 4 blocks): the table of the inodes is then in a
         * sector of its own, and writing it cannot erase the header.
         */
        KT_CHECK_INT_EQ(sb.info_start % 4, 0);
        KT_CHECK_INT_EQ(sb.info_blocks % 4, 0);

        /* The header carries the checksum of itself. */
        KT_CHECK((sb.features & MFS_FEATURE_CRC) != 0);
    }

    teardown();
}

/*
 * The property: a header that the storage lost in the middle is noticed,
 * and the volume that it describes is not mounted.
 */
static void test_header_crc(void)
{
    k_u8_t saved[MFS_BLOCK_SIZE];
    k_u8_t *info_block;

    ktest_begin("mfs: a damaged header is not trusted");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    info_block = g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE;
    memcpy(saved, info_block, MFS_BLOCK_SIZE);

    /* A bit that a brownout left half written. */
    info_block[sizeof(struct mfs_superblock) / 2] ^= 0x10;

    /* The check of the mounted volume reads the header again and names it. */
    {
        struct mfs_check_report report;

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK(report.errors > 0);
        KT_CHECK(report.first[0] != '\0');
        KT_CHECK_INT_EQ(strncmp(report.first, "header: ", 8), 0);
    }

    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_EINVAL);
    KT_CHECK(!mfs_is_mounted());

    /* The same volume, the header put back: everything is fine again. */
    memcpy(info_block, saved, MFS_BLOCK_SIZE);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    {
        struct mfs_check_report report;

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK_INT_EQ(report.first[0], '\0');
    }

    teardown();
}

/*
 * The property: the map of the free blocks reaches the device before the
 * record that points at the block. After a restart the directory must not
 * share its block with a file that was created later.
 */
static void test_mkdir_survives_restart(void)
{
    struct mfs_info before;
    struct mfs_info after;
    struct mfs_file file;
    char buffer[64];

    ktest_begin("mfs: a directory survives a restart without losing its block");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_mkdir("/logs"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mkdir("/logs/old"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);
    /* The restart of the board: only what reached the device stays. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    /* The blocks of the directories are still taken. */
    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.data_free, before.data_free);

    /* The directories are still there. */
    {
        struct mfs_dirent entries[8];

        memset(entries, 0, sizeof(entries));
        KT_CHECK_INT_EQ(mfs_list("/logs", entries, 8), MFS_OK);
        KT_CHECK_STR_EQ(entries[0].name, "old");
    }

    /* A new file must not be given the block of the directory. */
    KT_CHECK_INT_EQ(mfs_create("/logs/a.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/logs/a.txt", MFS_O_RDWR, &file), MFS_OK);
    memset(buffer, 'x', sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_write(&file, buffer, sizeof(buffer)), (int)sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    {
        struct mfs_dirent entries[8];
        int found_old = 0;
        int found_new = 0;
        k_size_t i;

        memset(entries, 0, sizeof(entries));
        KT_CHECK_INT_EQ(mfs_list("/logs", entries, 8), MFS_OK);

        for (i = 0; i < 8; i++) {
            if (strcmp(entries[i].name, "old") == 0) {
                found_old = 1;
            }
            if (strcmp(entries[i].name, "a.txt") == 0) {
                found_new = 1;
            }
        }

        /* Both are still there, the new file did not overwrite the entry. */
        KT_CHECK(found_old);
        KT_CHECK(found_new);
    }

    /* The directory still owns its block. */
    {
        struct mfs_inode inode;

        KT_CHECK_INT_EQ(mfs_stat("/logs", &inode, NULL), MFS_OK);
        KT_CHECK((inode.flags & MFS_INODE_DIR) != 0);
        KT_CHECK_INT_EQ(inode.entries, 2);
        KT_CHECK(inode.size > 0);
    }

    teardown();
}

/*
 * The property: a file opened with O_TRUNC gives its blocks back to the
 * map, and the map reaches the device.
 */
static void test_truncate_frees_blocks(void)
{
    struct mfs_info empty;
    struct mfs_info full;
    struct mfs_info after;
    struct mfs_file file;
    char buffer[3000];

    ktest_begin("mfs: a truncated file gives its blocks back");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&empty), MFS_OK);

    memset(buffer, 'a', sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_create("/big.bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, buffer, sizeof(buffer)), (int)sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&full), MFS_OK);
    KT_CHECK(full.data_free < empty.data_free);

    /* The truncation: the blocks of the chain go back to the map. */
    KT_CHECK_INT_EQ(mfs_open("/big.bin", MFS_O_RDWR | MFS_O_TRUNC, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.data_free, empty.data_free);

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

    /*
     * The chain of the file is what the check expects it to be: 5000 bytes
     * take five blocks of 1020 bytes, and the root holds the sixth.
     */
    {
        struct mfs_check_report report;

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK_INT_EQ(report.files, 1);
        KT_CHECK_INT_EQ(report.directories, 1);
        KT_CHECK_INT_EQ(report.used_blocks, 6);
        KT_CHECK_INT_EQ(report.first[0], '\0');
    }

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

    /*
     * The checker agrees with the tree the test has just built: two files,
     * the root and /etc, and no problem to name.
     */
    {
        struct mfs_check_report report;
        k_u32_t writes = g_volume.writes;

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK_INT_EQ(report.files, 2);
        KT_CHECK_INT_EQ(report.directories, 2);
        KT_CHECK(report.used_blocks > 0);
        KT_CHECK(report.free_blocks > 0);
        KT_CHECK_INT_EQ(report.first[0], '\0');
        KT_CHECK_INT_EQ(g_volume.writes, writes);
    }

    /*
     * The property: a record that a bad write left describing a file that
     * is larger than the whole volume is named by the check, and the check
     * does not repair the volume: the record stays the way it was found.
     */
    {
        struct mfs_check_report report;
        struct mfs_dirent readme;
        struct mfs_inode record;

        KT_CHECK_INT_EQ(mfs_stat("/readme", NULL, &readme), MFS_OK);
        memcpy(&record, inode_record(readme.inode), sizeof(record));
        record.size = 0xFFFFFFFFu;
        KT_CHECK_INT_EQ(store_inode_record(readme.inode, &record), 0);

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK(report.errors > 0);
        KT_CHECK(report.first[0] != '\0');
        KT_CHECK_INT_EQ(strncmp(report.first, "inode ", 6), 0);

        /* Read only: what the test broke is still broken, not repaired. */
        KT_CHECK_INT_EQ(memcmp(inode_record(readme.inode), &record, sizeof(record)), 0);
    }

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

/*
 * The property: a new object gets the mode of the design - 0644 for a
 * file, 0755 for a directory - and the owner 0. The type of the record
 * stays in the two low bits, where it always was.
 */
static void test_default_modes(void)
{
    struct mfs_inode inode;
    struct mfs_dirent entry;
    struct mfs_dirent entries[8];
    k_size_t i;
    int found = 0;

    ktest_begin("mfs: a new file is 0644 and a new directory is 0755");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    /* The root of a new volume is a directory like any other. */
    KT_CHECK_INT_EQ(mfs_stat("/", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MFS_MODE_DIR_DEFAULT);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 0);

    KT_CHECK_INT_EQ(mfs_create("/plain.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/plain.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MFS_MODE_DEFAULT);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 0);

    KT_CHECK_INT_EQ(mfs_mkdir("/sub"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/sub", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MFS_MODE_DIR_DEFAULT);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 0);

    /* The two low bits are still the type: nothing moved under them. */
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_USED, MFS_INODE_USED);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_DIR, MFS_INODE_DIR);

    /* The entry that a stat returns carries the same numbers. */
    memset(&entry, 0, sizeof(entry));
    KT_CHECK_INT_EQ(mfs_stat("/sub", NULL, &entry), MFS_OK);
    KT_CHECK_INT_EQ(entry.mode, MFS_MODE_DIR_DEFAULT);
    KT_CHECK_INT_EQ(entry.owner, 0);

    /* And so does a listing, without a stat per entry. */
    memset(entries, 0, sizeof(entries));
    KT_CHECK_INT_EQ(mfs_list("/", entries, 8), MFS_OK);
    for (i = 0; i < 8; i++) {
        if (strcmp(entries[i].name, "plain.txt") == 0) {
            found = 1;
            KT_CHECK_INT_EQ(entries[i].mode, MFS_MODE_DEFAULT);
            KT_CHECK_INT_EQ(entries[i].owner, 0);
        }
    }
    KT_CHECK(found);

    teardown();
}

/*
 * The property: chmod changes the nine bits of the mode and nothing else -
 * the type of the record and the owner stay where they were.
 */
static void test_chmod(void)
{
    struct mfs_inode inode;

    ktest_begin("mfs: chmod changes the mode and keeps the type and the owner");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/tool"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_chown("/tool", 42), MFS_OK);

    KT_CHECK_INT_EQ(mfs_chmod("/tool", 0755), MFS_OK);
    /* The mode is written into the table; a sync is the end of the change. */
    KT_CHECK_INT_EQ(mfs_sync(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/tool", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0755);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_USED, MFS_INODE_USED);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_DIR, 0);

    /* The mode nine bits are all modes, except the zero: see below. */
    KT_CHECK_INT_EQ(mfs_chmod("/tool", 0001), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/tool", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0001);

    /*
     * Zero means "the record predates the modes" in a volume of 0.3.x, so
     * chmod refuses it: a stored zero would come back as 0644 and the
     * caller would believe a rule that the filesystem does not keep.
     */
    KT_CHECK_INT_EQ(mfs_chmod("/tool", 0), MFS_EINVAL);
    KT_CHECK_INT_EQ(mfs_stat("/tool", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0001);

    KT_CHECK_INT_EQ(mfs_chmod("/tool", 0777), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/tool", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0777);

    /* A change of the mode does not touch the owner. */
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 42);

    /* chmod is not a way to make a file out of a directory. */
    KT_CHECK_INT_EQ(mfs_mkdir("/dir"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_chmod("/dir", 0700), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/dir", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_DIR, MFS_INODE_DIR);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0700);

    KT_CHECK_INT_EQ(mfs_chmod("/nope", 0644), MFS_ENOENT);

    teardown();
}

/*
 * The property: a mode or an owner that does not fit into its bits is
 * refused, and the record keeps what it had.
 */
static void test_chmod_chown_bounds(void)
{
    struct mfs_inode inode;

    ktest_begin("mfs: a mode outside 9 bits and an owner outside 15 are refused");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/bounds"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_chmod("/bounds", 0640), MFS_OK);
    KT_CHECK_INT_EQ(mfs_chown("/bounds", 7), MFS_OK);

    /* The first number that does not fit into the nine bits of the mode. */
    KT_CHECK_INT_EQ(mfs_chmod("/bounds", MFS_MODE_MASK + 1u), MFS_EINVAL);
    /* The mode of Linux has a sticky bit of its own, the design has not. */
    KT_CHECK_INT_EQ(mfs_chmod("/bounds", 01000u), MFS_EINVAL);
    KT_CHECK_INT_EQ(mfs_chmod("/bounds", 0xFFFFFFFFu), MFS_EINVAL);
    /* And zero means "the record predates the modes", not "no access". */
    KT_CHECK_INT_EQ(mfs_chmod("/bounds", 0), MFS_EINVAL);

    /* The record did not change: neither the mode nor the owner. */
    KT_CHECK_INT_EQ(mfs_stat("/bounds", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0640);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 7);

    /* The largest owner that fits is taken, the next one is refused. */
    KT_CHECK_INT_EQ(mfs_chown("/bounds", MFS_OWNER_MASK), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/bounds", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), MFS_OWNER_MASK);

    KT_CHECK_INT_EQ(mfs_chown("/bounds", MFS_OWNER_MASK + 1u), MFS_EINVAL);
    KT_CHECK_INT_EQ(mfs_chown("/bounds", 0xFFFFFFFFu), MFS_EINVAL);
    KT_CHECK_INT_EQ(mfs_stat("/bounds", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), MFS_OWNER_MASK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0640);

    KT_CHECK_INT_EQ(mfs_chown("/nope", 0), MFS_ENOENT);

    teardown();
}

/*
 * The property: the volume of 0.3.x has no modes in it, and it is mounted
 * the way it always was. A record without a mode is read as the mode of
 * the design for its type, and the volume is not rewritten to make that
 * true: the fallback is a rule of the reader, not a change of the data.
 */
static void test_volume_without_modes(void)
{
    struct mfs_inode inode;
    struct mfs_dirent entry;
    struct mfs_dirent entries[8];
    struct mfs_file file;
    k_u32_t old_dir_flags = MFS_INODE_USED | MFS_INODE_DIR;
    k_u32_t old_file_flags = MFS_INODE_USED;

    ktest_begin("mfs: a volume written before the modes still mounts");

    KT_CHECK_INT_EQ(setup(), MFS_OK);

    /* The root of a volume of 0.3.x: the type bits only, no mode, no owner. */
    memcpy(inode_record(MFS_ROOT_INODE), &old_dir_flags, sizeof(old_dir_flags));

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_is_mounted());

    /* A record of 0.3.x has no mode and no owner, and the check allows it. */
    {
        struct mfs_check_report report;

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK_INT_EQ(report.directories, 1);
    }

    /*
     * The record says nothing about the access, so the reader falls back
     * to the mode of the design: no object of an old volume is unreadable
     * and nothing had to be written at the mount.
     */
    KT_CHECK_INT_EQ(mfs_stat("/", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_TYPE_MASK, old_dir_flags);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MFS_MODE_DIR_DEFAULT);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 0);

    /* The volume takes new objects, and they carry the mode of the design. */
    KT_CHECK_INT_EQ(mfs_mkdir("/etc"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/etc/hosts"), MFS_OK);

    /* The file is made old the same way: a record of a volume of 0.3.x. */
    memset(&entry, 0, sizeof(entry));
    KT_CHECK_INT_EQ(mfs_stat("/etc/hosts", NULL, &entry), MFS_OK);
    memcpy(inode_record(entry.inode), &old_file_flags, sizeof(old_file_flags));

    KT_CHECK_INT_EQ(mfs_stat("/etc/hosts", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.flags & MFS_INODE_DIR, 0);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MFS_MODE_DEFAULT);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 0);

    /* A listing of the directory reads the same fallback. */
    memset(entries, 0, sizeof(entries));
    KT_CHECK_INT_EQ(mfs_list("/etc", entries, 8), MFS_OK);
    KT_CHECK_STR_EQ(entries[0].name, "hosts");
    KT_CHECK_INT_EQ(entries[0].mode, MFS_MODE_DEFAULT);
    KT_CHECK_INT_EQ(entries[0].owner, 0);

    /* A mode that is on the volume wins over the fallback. */
    KT_CHECK_INT_EQ(mfs_chmod("/etc/hosts", 0600), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/etc/hosts", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0600);

    /* The owner can be filled in on such a record, the mode stays. */
    KT_CHECK_INT_EQ(mfs_chown("/etc/hosts", 3), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/etc/hosts", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 3);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0600);

    KT_CHECK_INT_EQ(mfs_open("/etc/hosts", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "127.0.0.1", 9), 9);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* And the mode is on the volume, not only in the memory. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/etc/hosts", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), 0600);
    KT_CHECK_INT_EQ(mfs_inode_owner(&inode), 3);

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

/* The state of the two tasks of the test of the lock. */
static volatile int s_writers_failed;
static volatile int s_writers_done;

/*
 * The two writers update these counters at the same moment, and a plain
 * ++ loses one of the two updates now and then: the test then says that a
 * writer never finished, while both did. The counter is bumped by the
 * builtin, which is one instruction and no race.
 */
static void count_failure(void)
{
    (void)__atomic_add_fetch(&s_writers_failed, 1, __ATOMIC_SEQ_CST);
}

static void count_done(void)
{
    (void)__atomic_add_fetch(&s_writers_done, 1, __ATOMIC_SEQ_CST);
}

/*
 * Every task writes its own file, with pauses between the writes: the
 * tasks meet each other in the middle of their work, and the filesystem
 * has to keep them apart.
 */
static void writer_task(void *arg)
{
    char name[16];
    char buffer[64];
    struct mfs_file file;
    int id = (int)(intptr_t)arg;
    int i;

    memset(buffer, 'a' + id, sizeof(buffer));
    snprintf(name, sizeof(name), "/w%d.bin", id);

    if (mfs_create(name) != MFS_OK) {
        count_failure();
        return;
    }

    if (mfs_open(name, MFS_O_RDWR, &file) != MFS_OK) {
        count_failure();
        return;
    }

    for (i = 0; i < 20; i++) {
        if (mfs_write(&file, buffer, sizeof(buffer)) != (int)sizeof(buffer)) {
            count_failure();
            break;
        }
        k_sched_yield();
    }

    if (mfs_close(&file) != MFS_OK) {
        count_failure();
    }

    count_done();
}

/*
 * The property: two tasks that write their own files at the same time do
 * not see each other's work: neither of the files is given the blocks of
 * the other one, and both of them read back the way they were written.
 */
static void test_two_tasks_write(void)
{
    struct task *tasks[2];
    char name[16];
    char buffer[64];
    char back[64];
    struct mfs_file file;
    int id;
    int failures = 0;

    ktest_begin("mfs: two tasks that write files at the same time keep them apart");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    s_writers_failed = 0;
    s_writers_done = 0;

    for (id = 0; id < 2; id++) {
        char task_name[16];

        snprintf(task_name, sizeof(task_name), "writer%d", id);
        tasks[id] = task_create(task_name, writer_task, (void *)(intptr_t)id, (k_u8_t)(5 + id));
        KT_CHECK(tasks[id] != NULL);
    }

    for (id = 0; id < 2; id++) {
        if (tasks[id] != NULL) {
            /* The task is waited for: a finished task has to be taken back. */
            KT_CHECK_INT_EQ(task_wait(tasks[id]->pid, NULL, 5000), 0);
        }
    }

    KT_CHECK_INT_EQ(s_writers_done, 2);
    KT_CHECK_INT_EQ(s_writers_failed, 0);

    /* Every file holds its own byte, all the way through. */
    for (id = 0; id < 2; id++) {
        int i;
        int good = 1;

        snprintf(name, sizeof(name), "/w%d.bin", id);
        memset(buffer, 'a' + id, sizeof(buffer));

        KT_CHECK_INT_EQ(mfs_open(name, MFS_O_RDONLY, &file), MFS_OK);
        memset(back, 0, sizeof(back));
        for (i = 0; i < 20; i++) {
            if (mfs_read(&file, back, sizeof(back)) != (int)sizeof(back)) {
                good = 0;
                break;
            }
            if (memcmp(back, buffer, sizeof(back)) != 0) {
                good = 0;
                break;
            }
        }
        KT_CHECK_INT_EQ(good, 1);
        mfs_close(&file);
    }

    /* The volume is still in one piece after a restart. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/w0.bin", NULL, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/w1.bin", NULL, NULL), MFS_OK);

    teardown();
    failures = 0;
    (void)failures;
}

/*
 * The property: the flags of the open decide what the file is opened for.
 * "append" adds to the end of the file whatever the position of the
 * descriptor says, and a file that was opened to be written to cannot be
 * read from (and the other way around).
 */
static void test_append_and_access(void)
{
    struct mfs_file file;
    char buffer[32];

    ktest_begin("mfs: append adds to the end, the access is the one that was asked for");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    /* A file with "hello" in it. */
    KT_CHECK_INT_EQ(mfs_create("/a.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "hello", 5), 5);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* Opening with append starts at the end of the file. */
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_WRONLY | MFS_O_APPEND, &file), MFS_OK);
    KT_CHECK_INT_EQ((int)file.offset, 5);
    KT_CHECK_INT_EQ(mfs_write(&file, " world", 6), 6);

    /*
     * The position is moved back by hand: a file with "append" is still
     * written to its end.
     */
    file.offset = 0;
    KT_CHECK_INT_EQ(mfs_write(&file, "!", 1), 1);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* The file holds "hello world!" - the second write did not go to the head. */
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ((int)file.size, 12);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_read(&file, buffer, sizeof(buffer)), 12);
    KT_CHECK_STR_EQ(buffer, "hello world!");
    mfs_close(&file);

    /* A file opened to be written to cannot be read from. */
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK(mfs_read(&file, buffer, sizeof(buffer)) < 0);
    mfs_close(&file);

    /* A file opened to be read from cannot be written to. */
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK(mfs_write(&file, "x", 1) < 0);
    mfs_close(&file);

    teardown();
}

/*
 * The property: a volume is found by the name of its device, formatted when
 * the medium was never written and mounted at a point of the tree. The
 * filesystem itself knows no platform: the device is whatever the registry
 * holds under that name.
 */
static void test_vfs_mount(void)
{
    struct mfs_ram_device volume;
    struct k_blockdev device;
    struct mfs_file file;
    char buffer[8];

    ktest_begin("mfs: the volume of a device is mounted by name");

    KT_CHECK_INT_EQ(mfs_ram_init(&volume, TEST_BLOCKS), 0);
    mfs_ram_device(&volume, &device);

    /* The medium was never written: an erased flash is all ones. */
    memset(volume.data, 0xFF, (size_t)TEST_BLOCKS * MFS_BLOCK_SIZE);
    KT_CHECK_INT_EQ(k_blockdev_register(&device), 0);

    /* The device is not there: the mount says so and touches nothing. */
    KT_CHECK(mfs_vfs_mount("/", "nope") != 0);

    KT_CHECK_INT_EQ(mfs_vfs_mount("/", "ram0"), 0);
    KT_CHECK(mfs_is_mounted());
    KT_CHECK_INT_EQ(mfs_create("/first.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/first.txt", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "hello", 5), 5);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* The volume that the first mount made is mounted, not made again. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_vfs_mount("/", "ram0"), 0);
    KT_CHECK_INT_EQ(mfs_open("/first.txt", MFS_O_RDONLY, &file), MFS_OK);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_read(&file, buffer, sizeof(buffer)), 5);
    KT_CHECK_STR_EQ(buffer, "hello");
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* A format of the volume: the file is gone, the volume is usable. */
    KT_CHECK_INT_EQ(mfs_vfs_format("ram0"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_vfs_mount("/", "ram0"), 0);
    KT_CHECK_INT_EQ(mfs_open("/first.txt", MFS_O_RDONLY, &file), MFS_ENOENT);

    /* The volume of this test is not the one of setup(): it is freed here. */
    mfs_unmount();
    mfs_ram_free(&volume);
}

void test_mfs(void)
{
    test_format();
    test_layout();
    test_header_crc();
    test_mkdir_survives_restart();
    test_truncate_frees_blocks();
    test_two_tasks_write();
    test_append_and_access();
    test_files();
    test_big_file();
    test_directories();
    test_unlink();
    test_default_modes();
    test_chmod();
    test_chmod_chown_bounds();
    test_volume_without_modes();
    test_persistence();
    test_vfs_mount();
}
