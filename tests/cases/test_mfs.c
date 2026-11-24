#include <stdatomic.h>

#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/clock.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
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

static k_u32_t test_crc32(const void *data, k_size_t size);

/*
 * The address of a record of the table of the inodes in the volume of the
 * test. The layout is computed the way the format computes it, so the test
 * can put a record of an older version into a volume of today: a version 3
 * volume numbers its records from the top of the table, which is anchored
 * at the map next to INFO (ADR 0038).
 */
static k_u8_t *inode_record(k_u32_t index)
{
    struct mfs_superblock sb;
    k_u32_t per_block = MFS_BLOCK_SIZE / MFS_INODE_SIZE;
    k_u32_t slots = index + 1u;
    k_u32_t block;
    k_u32_t slot;

    memcpy(&sb, g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE, sizeof(sb));

    block = (sb.info_start - sb.bitmap_blocks) - (slots + per_block - 1u) / per_block;
    slot = (per_block - slots % per_block) % per_block;

    return g_volume.data + (size_t)block * MFS_BLOCK_SIZE + (size_t)slot * MFS_INODE_SIZE;
}

/*
 * Put a record into the table the way a writer of the device would: the
 * test goes through the block device and not through the array in the
 * memory, so the record reaches the volume the way it does on the board.
 * The record of a version 3 volume carries its checksum, and a record
 * without one is what the check of the volume calls a torn write.
 */
static int store_inode_record(k_u32_t index, const struct mfs_inode *inode)
{
    k_u8_t *where = inode_record(index);
    k_u8_t block[MFS_BLOCK_SIZE];
    size_t offset = (size_t)(where - g_volume.data);
    k_u32_t sector = (k_u32_t)(offset / MFS_BLOCK_SIZE);
    size_t at = offset % MFS_BLOCK_SIZE;
    k_u32_t crc;

    if (g_device.ops->read(g_device.ctx, sector, block, 1) != 0) {
        return -1;
    }

    memset(block + at, 0, MFS_INODE_SIZE);
    memcpy(block + at, inode, sizeof(*inode));

    crc = test_crc32(block + at, MFS_INODE_SIZE - sizeof(k_u32_t));
    memcpy(block + at + MFS_INODE_SIZE - sizeof(k_u32_t), &crc, sizeof(crc));

    return g_device.ops->write(g_device.ctx, sector, block, 1);
}

/*
 * The clock of the tests: a setter that takes every moment, so that a
 * record can be stamped with a known number. k_clock_now_unix() adds the
 * uptime that passed since the set, which is a second or two at most in a
 * test - the moments below are chosen far enough apart for it.
 */
static int test_clock_set(k_u64_t unix_seconds)
{
    (void)unix_seconds;
    return 0;
}

static k_u32_t test_crc32(const void *data, k_size_t size)
{
    const k_u8_t *bytes = (const k_u8_t *)data;
    k_u32_t crc = 0xFFFFFFFFu;
    k_size_t i;
    int bit;

    for (i = 0; i < size; i++) {
        crc ^= bytes[i];

        for (bit = 0; bit < 8; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

/*
 * Turn the volume of the test into the version 1 that came before the
 * times: the same data, the same directories, and the records in the 16
 * bytes they had, in the place a version 1 volume keeps them.
 *
 * The two layouts differ: a version 3 volume keeps the map at the top of
 * META and numbers the records from there downwards, a version 1 volume
 * keeps the map at the start of META and the table after it. The records are
 * read into the memory first, because the smaller table moves over the bytes
 * of the larger one, and the map is copied to its old place with them.
 */
static void volume_to_legacy(void)
{
    struct mfs_superblock sb;
    struct mfs_inode records[256];
    bool used[256];
    k_u8_t map[MFS_BLOCK_SIZE];
    k_u32_t bitmap_blocks;
    k_u32_t capacity;
    k_u32_t i;

    memcpy(&sb, g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE, sizeof(sb));
    bitmap_blocks = (sb.data_blocks + 8u * MFS_BLOCK_SIZE - 1u) / (8u * MFS_BLOCK_SIZE);
    capacity = sb.inode_records - 1u;
    if (capacity >= K_ARRAY_SIZE(records)) {
        capacity = K_ARRAY_SIZE(records) - 1u;
    }

    memset(records, 0, sizeof(records));
    memset(used, 0, sizeof(used));

    for (i = 1; i <= capacity; i++) {
        memcpy(&records[i], inode_record(i), sizeof(records[i]));
        used[i] = (records[i].flags & MFS_INODE_USED) != 0u;
    }

    /* The map of the free blocks moves from the top of META to its start. */
    memcpy(map, g_volume.data + (size_t)(sb.info_start - bitmap_blocks) * MFS_BLOCK_SIZE,
           MFS_BLOCK_SIZE);

    memset(g_volume.data + (size_t)sb.meta_start * MFS_BLOCK_SIZE, 0,
           (size_t)sb.meta_blocks * MFS_BLOCK_SIZE);
    memcpy(g_volume.data + (size_t)sb.meta_start * MFS_BLOCK_SIZE, map, MFS_BLOCK_SIZE);

    {
        k_u8_t *table = g_volume.data + (size_t)(sb.meta_start + bitmap_blocks) * MFS_BLOCK_SIZE;

        for (i = 1; i <= capacity; i++) {
            if (used[i]) {
                memcpy(table + (size_t)i * MFS_INODE_SIZE_LEGACY, &records[i],
                       MFS_INODE_SIZE_LEGACY);
            }
        }
    }

    sb.version = MFS_VERSION_LEGACY;
    sb.features &= (k_u32_t) ~(MFS_FEATURE_TIME | MFS_FEATURE_RECORD_CRC);
    sb.bitmap_blocks = 0;
    sb.inode_blocks = 0;
    sb.inode_size = 0;
    sb.inode_records = 0;
    sb.crc = test_crc32(&sb, MFS_HEADER_CRC_SPAN);

    memcpy(g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE, &sb, sizeof(sb));
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

    /*
     * The property: a directory that is already there takes no block when
     * it is made again. The map of the free blocks reaches the device
     * before the record that points at the block of a new directory, so a
     * block that is taken and given back only in the memory would stay
     * taken on the device, and a power cut would lose it for good.
     */
    {
        struct mfs_check_report report;

        KT_CHECK_INT_EQ(mfs_mkdir("/logs"), MFS_EEXIST);

        /*
         * A power cut: the state of the memory is gone and the mount reads
         * the map of the free blocks from the device again.
         */
        KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK_INT_EQ(report.first[0], '\0');
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

/*
 * The property: a file cut to a smaller size keeps its first bytes, reads
 * zeros after a growth, and the map of the free blocks is the one of a
 * sound volume when the check is asked.
 */
static void test_truncate_to_size(void)
{
    static char content[40u * 1024u];
    static char check[12u * 1024u];
    struct mfs_check_report report;
    struct mfs_inode inode;
    struct mfs_file file;
    k_size_t i;
    int rc;

    ktest_begin("mfs: a file of forty kilobytes is cut to ten and grows back");

    for (i = 0; i < sizeof(content); i++) {
        content[i] = (char)(i & 0xFF);
    }

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/cut.bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/cut.bin", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, content, sizeof(content)), (int)sizeof(content));

    /* The cut is made through the open file, as ftruncate does it. */
    KT_CHECK_INT_EQ(mfs_truncate(&file, 10u * 1024u), MFS_OK);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_stat("/cut.bin", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 10u * 1024u);

    KT_CHECK_INT_EQ(mfs_open("/cut.bin", MFS_O_RDONLY, &file), MFS_OK);
    memset(check, 0, sizeof(check));
    KT_CHECK_INT_EQ(mfs_read(&file, check, sizeof(check)), 10 * 1024);
    KT_CHECK(memcmp(content, check, 10u * 1024u) == 0);
    /* The bytes past the new size are not the file any more. */
    KT_CHECK_INT_EQ(mfs_read(&file, check, sizeof(check)), 0);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* A growth reads as zeros, including the tail of the block that held
     * the old size. */
    KT_CHECK_INT_EQ(mfs_open("/cut.bin", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_truncate(&file, 12u * 1024u), MFS_OK);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_open("/cut.bin", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_read(&file, check, sizeof(check)), 12 * 1024);
    KT_CHECK(memcmp(content, check, 10u * 1024u) == 0);
    for (i = 10u * 1024u; i < 12u * 1024u; i++) {
        KT_CHECK_INT_EQ(check[i], 0);
    }
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.first[0], '\0');

    /* The format refuses a size that its record cannot hold. */
    KT_CHECK_INT_EQ(mfs_open("/cut.bin", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_truncate(&file, (k_u64_t)0x100000000ull), MFS_E2BIG);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* A reader cannot cut the file. */
    KT_CHECK_INT_EQ(mfs_open("/cut.bin", MFS_O_RDONLY, &file), MFS_OK);
    rc = mfs_truncate(&file, 1u);
    KT_CHECK_INT_EQ(rc, MFS_EINVAL);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

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

    /* The two calls of POSIX: unlink of a directory is refused, and rmdir of
     * a directory that still holds an entry is not empty. */
    KT_CHECK_INT_EQ(mfs_mkdir("/dir"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/dir/file"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_unlink("/dir"), MFS_EISDIR);
    KT_CHECK_INT_EQ(mfs_rmdir("/dir"), MFS_ENOTEMPTY);
    KT_CHECK_INT_EQ(mfs_unlink("/dir/file"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rmdir("/dir"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rmdir("/dir"), MFS_ENOENT);
    KT_CHECK_INT_EQ(mfs_create("/plain"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rmdir("/plain"), MFS_ENOTDIR);
    KT_CHECK_INT_EQ(mfs_unlink("/plain"), MFS_OK);

    teardown();
}

/*
 * The property: one call moves a name and not the bytes, a file moves to
 * another directory of the volume, a directory stays in its parent, and the
 * two calls of POSIX remove exactly one kind of object each.
 */
static void test_rename(void)
{
    struct mfs_file file;
    struct mfs_inode inode;
    char buffer[32];

    ktest_begin("mfs: a name moves and a directory is removed by rmdir");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/one.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/one.txt", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "hello", 5), 5);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_rename("/one.txt", "/two.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/one.txt", &inode, NULL), MFS_ENOENT);
    KT_CHECK_INT_EQ(mfs_stat("/two.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 5);
    KT_CHECK_INT_EQ(mfs_open("/two.txt", MFS_O_RDONLY, &file), MFS_OK);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(mfs_read(&file, buffer, sizeof(buffer)), 5);
    KT_CHECK_STR_EQ(buffer, "hello");
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* A file moves between directories; a directory only inside its own
     * parent, and a name at the destination that is a file is replaced. */
    KT_CHECK_INT_EQ(mfs_mkdir("/dir"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mkdir("/other"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rename("/two.txt", "/dir/two.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/two.txt", &inode, NULL), MFS_ENOENT);
    KT_CHECK_INT_EQ(mfs_stat("/dir/two.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rename("/dir", "/other/dir"), MFS_ENOTSUP);
    KT_CHECK_INT_EQ(mfs_rename("/dir", "/other"), MFS_EISDIR);

    KT_CHECK_INT_EQ(mfs_create("/dir/other.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rename("/dir/two.txt", "/dir/other.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/dir/two.txt", &inode, NULL), MFS_ENOENT);
    KT_CHECK_INT_EQ(mfs_stat("/dir/other.txt", &inode, NULL), MFS_OK);

    /* The two removals of POSIX do not stand in for each other. */
    KT_CHECK_INT_EQ(mfs_unlink("/dir"), MFS_EISDIR);
    KT_CHECK_INT_EQ(mfs_rmdir("/dir"), MFS_ENOTEMPTY);
    KT_CHECK_INT_EQ(mfs_unlink("/dir/other.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rmdir("/dir/other.txt"), MFS_ENOENT);
    KT_CHECK_INT_EQ(mfs_rmdir("/dir"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/plain"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rmdir("/plain"), MFS_ENOTDIR);
    KT_CHECK_INT_EQ(mfs_unlink("/plain"), MFS_OK);

    /* What the calls wrote is on the medium and not only in memory. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/other", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/dir", &inode, NULL), MFS_ENOENT);

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

    /*
     * The root of a volume of 0.3.x: the type bits only, no mode, no owner.
     * The record goes back through the device with the checksum of the
     * format of today, so that what is old about it is the flags and not a
     * record that a power cut tore (ADR 0038).
     */
    {
        struct mfs_inode root;

        memcpy(&root, inode_record(MFS_ROOT_INODE), sizeof(root));
        root.flags = old_dir_flags;
        KT_CHECK_INT_EQ(store_inode_record(MFS_ROOT_INODE, &root), 0);
    }

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
    {
        struct mfs_inode old;

        memcpy(&old, inode_record(entry.inode), sizeof(old));
        old.flags = old_file_flags;
        KT_CHECK_INT_EQ(store_inode_record(entry.inode, &old), 0);
    }

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

/*
 * The property: the record carries the moments of the change, they move
 * when the content or the record changes, and nothing rewrites them on the
 * way to the device. The clock is the one of the kernel, and the fake of
 * the test is a setter like the RTC of the board.
 */
static void test_timestamps(void)
{
    struct mfs_inode inode;
    struct mfs_file file;
    k_u64_t created;
    k_u64_t written;
    k_u64_t truncated;
    k_u64_t directory;
    k_u64_t with_child;
    k_u64_t without_child;
    k_u64_t after_chmod;

    ktest_begin("mfs: create, write and truncate move the moments of a record");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    k_clock_register(test_clock_set);
    KT_CHECK_INT_EQ(k_clock_set_unix(1000), 0);

    KT_CHECK_INT_EQ(mfs_create("/time.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/time.txt", &inode, NULL), MFS_OK);
    created = inode.mtime;
    KT_CHECK(created >= 1000);
    KT_CHECK_INT_EQ(inode.ctime, created);

    /* A write moves both: the content and the record are new. */
    KT_CHECK_INT_EQ(k_clock_set_unix(2000), 0);
    KT_CHECK_INT_EQ(mfs_open("/time.txt", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "hello", 5), 5);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/time.txt", &inode, NULL), MFS_OK);
    written = inode.mtime;
    KT_CHECK(written > created);
    KT_CHECK_INT_EQ(inode.ctime, written);

    /* A truncation is a change of the content, even when there was little. */
    KT_CHECK_INT_EQ(k_clock_set_unix(3000), 0);
    KT_CHECK_INT_EQ(mfs_open("/time.txt", MFS_O_RDWR | MFS_O_TRUNC, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/time.txt", &inode, NULL), MFS_OK);
    truncated = inode.mtime;
    KT_CHECK(truncated > written);
    KT_CHECK_INT_EQ(inode.ctime, truncated);

    /* A directory is stamped when it is made and when its entries change. */
    KT_CHECK_INT_EQ(k_clock_set_unix(4000), 0);
    KT_CHECK_INT_EQ(mfs_mkdir("/time-dir"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/time-dir", &inode, NULL), MFS_OK);
    directory = inode.mtime;
    KT_CHECK(directory > truncated);

    KT_CHECK_INT_EQ(k_clock_set_unix(5000), 0);
    KT_CHECK_INT_EQ(mfs_create("/time-dir/child"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/time-dir", &inode, NULL), MFS_OK);
    with_child = inode.mtime;
    KT_CHECK(with_child > directory);

    KT_CHECK_INT_EQ(k_clock_set_unix(6000), 0);
    KT_CHECK_INT_EQ(mfs_unlink("/time-dir/child"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/time-dir", &inode, NULL), MFS_OK);
    without_child = inode.mtime;
    KT_CHECK(without_child > with_child);

    /*
     * A change of the record alone moves ctime and leaves mtime where the
     * content left it.
     */
    KT_CHECK_INT_EQ(k_clock_set_unix(7000), 0);
    KT_CHECK_INT_EQ(mfs_chmod("/time.txt", 0600), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/time.txt", &inode, NULL), MFS_OK);
    after_chmod = inode.ctime;
    KT_CHECK(after_chmod > truncated);
    KT_CHECK_INT_EQ(inode.mtime, truncated);

    /* The moments are on the volume, not only in the memory of the mount. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/time.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.mtime, truncated);
    KT_CHECK_INT_EQ(inode.ctime, after_chmod);

    /*
     * A moment past 2^32 is a moment of this format: the field is 64 bits
     * wide, and the year 2038 is not the end of it.
     */
    KT_CHECK_INT_EQ(k_clock_set_unix(0x100000000ull), 0);
    KT_CHECK_INT_EQ(mfs_create("/wide-time.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/wide-time.txt", &inode, NULL), MFS_OK);
    KT_CHECK(inode.mtime >= 0x100000000ull);
    created = inode.mtime;

    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/wide-time.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.mtime, created);

    k_clock_register(NULL);

    teardown();
}

/*
 * The property: the volume of the version before the times mounts, keeps
 * its data and reports no time - zero, which is "unknown" and not 1970.
 * Nothing is rewritten to make the old record new, and a write to it stays
 * timeless because the 16 bytes of its record have no room for a moment.
 */
static void test_legacy_volume_without_time(void)
{
    struct mfs_superblock sb;
    struct mfs_inode inode;
    struct mfs_file file;
    struct mfs_check_report report;
    struct vfs_statx info;

    ktest_begin("mfs: a version 1 volume mounts and reports no time");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    k_clock_register(test_clock_set);
    KT_CHECK_INT_EQ(k_clock_set_unix(1234), 0);

    KT_CHECK_INT_EQ(mfs_create("/old.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/old.txt", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "old volume", 10), 10);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* The medium of a release before the times. */
    mfs_unmount();
    volume_to_legacy();

    memcpy(&sb, g_volume.data + (size_t)(TEST_BLOCKS - 1) * MFS_BLOCK_SIZE, sizeof(sb));
    KT_CHECK_INT_EQ(sb.version, MFS_VERSION_LEGACY);
    KT_CHECK_INT_EQ(sb.features & MFS_FEATURE_TIME, 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/old.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 10);
    KT_CHECK_INT_EQ(inode.mtime, 0);
    KT_CHECK_INT_EQ(inode.ctime, 0);

    KT_CHECK_INT_EQ(mfs_stat("/", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.mtime, 0);
    KT_CHECK_INT_EQ(inode.ctime, 0);

    /* A write to a legacy record has nowhere to put a moment. */
    KT_CHECK_INT_EQ(k_clock_set_unix(9999), 0);
    KT_CHECK_INT_EQ(mfs_open("/old.txt", MFS_O_WRONLY | MFS_O_APPEND, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, " again", 6), 6);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/old.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 16);
    KT_CHECK_INT_EQ(inode.mtime, 0);
    KT_CHECK_INT_EQ(inode.ctime, 0);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.first[0], '\0');

    /* Through the VFS the same record answers the zeros of "unknown". */
    KT_CHECK_INT_EQ(mfs_vfs_register("/"), 0);
    memset(&info, 0, sizeof(info));
    KT_CHECK_INT_EQ(vfs_statx("/old.txt", &info), 0);
    KT_CHECK_INT_EQ(info.size, 16);
    KT_CHECK_INT_EQ(info.mtime, 0);
    KT_CHECK_INT_EQ(info.ctime, 0);
    KT_CHECK_INT_EQ(vfs_umount("/"), 0);

    k_clock_register(NULL);

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
static atomic_int s_writers_failed;
static atomic_int s_writers_done;

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

    /* The volume of this test is not the one of setup(): it is freed here,
     * and the device leaves the registry before its stack frame does - a
     * later test that looks a device up must not walk into it. */
    mfs_unmount();
    KT_CHECK_INT_EQ(k_blockdev_unregister(&device), 0);
    mfs_ram_free(&volume);
}

/*
 * The property: a directory is opened for reading and its entries come
 * through the descriptor one call at a time. Without the flag a directory is
 * still not a file, and a file opened with the flag is not a directory.
 */
static void test_directory_descriptor(void)
{
    struct mfs_dirent entries[8];
    struct mfs_file file;
    k_size_t count = 0;

    ktest_begin("mfs: a directory is read through its descriptor");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_mkdir("/etc"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/etc/config.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/readme"), MFS_OK);

    /* A plain open of a directory is still EISDIR. */
    KT_CHECK_INT_EQ(mfs_open("/", MFS_O_RDONLY, &file), MFS_EISDIR);

    /* The flag of a directory goes with the read access only. */
    KT_CHECK_INT_EQ(mfs_open("/", MFS_O_RDONLY | MFS_O_DIRECTORY | MFS_O_WRONLY, &file),
                    MFS_EINVAL);
    KT_CHECK_INT_EQ(mfs_open("/readme", MFS_O_RDONLY | MFS_O_DIRECTORY, &file), MFS_ENOTDIR);

    KT_CHECK_INT_EQ(mfs_open("/", MFS_O_RDONLY | MFS_O_DIRECTORY, &file), MFS_OK);

    /* One entry at a time: the position is the one of the open file. */
    KT_CHECK_INT_EQ(mfs_readdir(&file, entries, 1, &count), MFS_OK);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_STR_EQ(entries[0].name, "etc");
    KT_CHECK_INT_EQ(entries[0].type & MFS_INODE_DIR, MFS_INODE_DIR);

    KT_CHECK_INT_EQ(mfs_readdir(&file, entries, 8, &count), MFS_OK);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_STR_EQ(entries[0].name, "readme");
    KT_CHECK_INT_EQ(entries[0].type & MFS_INODE_DIR, 0);

    /* The end of the directory is an empty answer, not an error. */
    KT_CHECK_INT_EQ(mfs_readdir(&file, entries, 8, &count), MFS_OK);
    KT_CHECK_INT_EQ(count, 0);

    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* A file has no entries to give, however it was opened. */
    KT_CHECK_INT_EQ(mfs_open("/readme", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_readdir(&file, entries, 8, &count), MFS_ENOTDIR);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    teardown();
}

void test_mfs(void)
{
    test_format();
    test_layout();
    test_header_crc();
    test_mkdir_survives_restart();
    test_truncate_frees_blocks();
    test_truncate_to_size();
    test_two_tasks_write();
    test_append_and_access();
    test_files();
    test_big_file();
    test_directories();
    test_unlink();
    test_rename();
    test_default_modes();
    test_chmod();
    test_chmod_chown_bounds();
    test_volume_without_modes();
    test_timestamps();
    test_legacy_volume_without_time();
    test_persistence();
    test_vfs_mount();
    test_directory_descriptor();
}
