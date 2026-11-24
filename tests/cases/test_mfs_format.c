/*
 * Micro OS — the format of the volume of MicroFS, version 2 and 3.
 *
 * The volume of the board is made by `mkfs` and by the mount of a blank
 * device; the volumes that came before it have to keep mounting. This file
 * is the property of ADR 0038: a volume of version 1 and of version 2 is
 * read the way it always was, a volume of version 3 is made and checked,
 * the table of the records grows when the records run out instead of being
 * a share of the volume, every record carries a checksum, and a record that
 * a power cut left half written is named by the check, repaired by `fsck`,
 * and costs nothing but the object it was.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

/* The size of the volume of the tests: 512 blocks of 1 KB. */
#define FORMAT_BLOCKS 512

/* The bytes of the record of a version 3 volume. */
#define FORMAT_RECORD MFS_INODE_SIZE

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;
static struct mfs_superblock g_sb;

/* --- the volume of the test --- */

static k_u32_t test_crc32(const void *data, k_size_t size)
{
    const k_u8_t *bytes = (const k_u8_t *)data;
    k_u32_t crc = 0xFFFFFFFFu;
    k_size_t i;
    int bit;

    for (i = 0; i < size; i++) {
        crc ^= bytes[i];

        for (bit = 0; bit < 8; bit++) {
            crc = ((crc & 1u) != 0u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }

    return crc ^ 0xFFFFFFFFu;
}

static int volume_setup(k_u32_t records)
{
    int rc = mfs_ram_init(&g_volume, FORMAT_BLOCKS);

    if (rc != 0) {
        return rc;
    }

    mfs_ram_device(&g_volume, &g_device);
    rc = mfs_format(&g_device, records);

    memcpy(&g_sb, g_volume.data + (size_t)(FORMAT_BLOCKS - 1) * MFS_BLOCK_SIZE, sizeof(g_sb));

    return rc;
}

static void volume_teardown(void)
{
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static k_u8_t *volume_block(k_u32_t block)
{
    return g_volume.data + (size_t)block * MFS_BLOCK_SIZE;
}

/*
 * Where the record of a version 3 volume lies: the table is anchored at the
 * map next to INFO and grows downwards, so the record 0 is the last slot of
 * the top block (ADR 0038).
 */
static k_u32_t record_block(k_u32_t index)
{
    k_u32_t per_block = MFS_BLOCK_SIZE / FORMAT_RECORD;
    k_u32_t slots = index + 1u;

    return (g_sb.info_start - g_sb.bitmap_blocks) - (slots + per_block - 1u) / per_block;
}

static k_u32_t record_offset(k_u32_t index)
{
    k_u32_t per_block = MFS_BLOCK_SIZE / FORMAT_RECORD;

    return ((per_block - (index + 1u) % per_block) % per_block) * FORMAT_RECORD;
}

/* Read and write one record through the device, the way a writer does. */
static bool record_read(k_u32_t index, struct mfs_inode *out)
{
    if (index == 0u || index > g_sb.inode_records) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out, volume_block(record_block(index)) + record_offset(index), sizeof(struct mfs_inode));

    return true;
}

/* --- the volume of an older version --- */

/*
 * Turn the volume of the test into the version 2 that came before the
 * group and the checksum, or the version 1 before the times: the same data,
 * the same directories, the records in the bytes they had and in the place
 * that version keeps them.
 *
 * A version 1 and 2 volume keeps the map of the free blocks at the start of
 * META and the table after it, and the table takes the whole of META; a
 * version 3 volume keeps the map at the top and the table below it. The
 * records are read into the memory first, because the smaller table moves
 * over the bytes of the larger one.
 */
static void volume_to(k_u32_t version)
{
    struct mfs_inode records[512];
    k_u8_t map[MFS_BLOCK_SIZE];
    k_u8_t *table;
    k_u32_t bitmap_blocks = (g_sb.data_blocks + 8u * MFS_BLOCK_SIZE - 1u) / (8u * MFS_BLOCK_SIZE);
    k_u32_t capacity = g_sb.inode_records - 1u;
    k_u32_t size = (version == MFS_VERSION_LEGACY) ? MFS_INODE_SIZE_LEGACY : MFS_INODE_SIZE_TIME;
    k_u32_t i;

    if (capacity >= K_ARRAY_SIZE(records)) {
        capacity = K_ARRAY_SIZE(records) - 1u;
    }

    if (bitmap_blocks > 1u) {
        /* The test volume is small enough for one block of the map. */
        KT_CHECK(bitmap_blocks == 1u);
        return;
    }

    memset(records, 0, sizeof(records));

    for (i = 1; i <= capacity; i++) {
        KT_CHECK(record_read(i, &records[i]));
    }

    /* The map moves from the top of META to its start, and the table after
     * it is written in the bytes the older version has. */
    memcpy(map, volume_block(g_sb.info_start - bitmap_blocks), MFS_BLOCK_SIZE);
    memset(volume_block(g_sb.meta_start), 0, (size_t)g_sb.meta_blocks * MFS_BLOCK_SIZE);
    memcpy(volume_block(g_sb.meta_start), map, MFS_BLOCK_SIZE);

    table = volume_block(g_sb.meta_start + bitmap_blocks);

    for (i = 1; i <= capacity; i++) {
        if ((records[i].flags & MFS_INODE_USED) != 0u) {
            memcpy(table + (size_t)i * size, &records[i], size);
        }
    }

    g_sb.version = version;
    g_sb.features = MFS_FEATURE_CRC | ((version >= MFS_VERSION_TIME) ? MFS_FEATURE_TIME : 0u);
    g_sb.bitmap_blocks = 0;
    g_sb.inode_blocks = 0;
    g_sb.inode_size = 0;
    g_sb.inode_records = 0;
    g_sb.crc = test_crc32(&g_sb, MFS_HEADER_CRC_SPAN);

    memcpy(volume_block(FORMAT_BLOCKS - 1u), &g_sb, sizeof(g_sb));
}

/* --- the properties --- */

static void test_format_version(void)
{
    struct mfs_info info;
    struct mfs_check_report report;

    ktest_begin("mfs format: a new volume is of version 3 and the table is sized by records");

    KT_CHECK_INT_EQ(volume_setup(32), MFS_OK);

    /* The header of the volume says what the format of today is. */
    KT_CHECK_INT_EQ(g_sb.magic, MFS_MAGIC);
    KT_CHECK_INT_EQ(g_sb.version, MFS_VERSION);
    KT_CHECK_INT_EQ(g_sb.block_size, MFS_BLOCK_SIZE);
    KT_CHECK_INT_EQ(g_sb.inode_size, MFS_INODE_SIZE);
    KT_CHECK_INT_EQ(g_sb.features & MFS_FEATURE_TIME, MFS_FEATURE_TIME);
    KT_CHECK_INT_EQ(g_sb.features & MFS_FEATURE_RECORD_CRC, MFS_FEATURE_RECORD_CRC);

    /* The map and the table fill META and nothing else: the table is the
     * records asked for and a reserve of a quarter of them, 64 at least. */
    KT_CHECK_INT_EQ(g_sb.bitmap_blocks + g_sb.inode_blocks, g_sb.meta_blocks);
    KT_CHECK_INT_EQ(g_sb.inode_records, 32u + 64u);
    KT_CHECK_INT_EQ(g_sb.inode_blocks, (32u + 64u) / (MFS_BLOCK_SIZE / MFS_INODE_SIZE));
    KT_CHECK(g_sb.meta_blocks < 16u);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK_INT_EQ(info.inodes_total, 32u + 64u - 1u);
    KT_CHECK_INT_EQ(info.inodes_used, 1u); /* the root */
    KT_CHECK_INT_EQ(info.meta_blocks, g_sb.meta_blocks);

    /* A volume just made is sound, and the check says so and writes nothing. */
    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.directories, 1u);
    KT_CHECK_INT_EQ(report.first[0], '\0');

    /* Every record of the table is one that a writer writes: the checksum of
     * the whole table holds, the record of the root included. */
    {
        struct mfs_inode root;
        k_u8_t *at = volume_block(record_block(MFS_ROOT_INODE)) + record_offset(MFS_ROOT_INODE);
        k_u32_t crc;

        memcpy(&crc, at + FORMAT_RECORD - sizeof(k_u32_t), sizeof(crc));
        KT_CHECK_INT_EQ(crc, test_crc32(at, FORMAT_RECORD - sizeof(k_u32_t)));
        KT_CHECK_INT_EQ(record_read(MFS_ROOT_INODE, &root), true);
        KT_CHECK_INT_EQ(root.flags & MFS_INODE_TYPE_MASK, MFS_INODE_USED | MFS_INODE_DIR);
        KT_CHECK_INT_EQ(mfs_record_crc(at), crc);
    }

    volume_teardown();
}

static void test_version_time_volume(void)
{
    struct mfs_inode inode;
    struct mfs_file file;
    struct mfs_check_report report;

    ktest_begin("mfs format: a volume of version 2 is read and checked");

    KT_CHECK_INT_EQ(volume_setup(32), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/old.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/old.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "0123456789", 10), 10);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mkdir("/old"), MFS_OK);
    mfs_unmount();

    /* The volume of 0.4: 32-byte records with the times and no group. */
    volume_to(MFS_VERSION_TIME);
    KT_CHECK_INT_EQ(g_sb.version, MFS_VERSION_TIME);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/old.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 10u);
    KT_CHECK_INT_EQ(mfs_inode_group(&inode), 0u);
    /* A record of version 2 has the times of its format, which the host
     * build of the tests may hold as zero: the test does not read a date,
     * it reads that the fields are there and that the volume takes a write
     * without touching the record that follows the one it writes. */

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.files, 1u);
    KT_CHECK_INT_EQ(report.directories, 2u);

    /* The volume of version 2 takes a write, in the record it has. */
    KT_CHECK_INT_EQ(mfs_open("/old.txt", MFS_O_WRONLY | MFS_O_APPEND, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "abc", 3), 3);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/old.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 13u);

    /* A group is what a version 2 volume cannot keep, and it says so. */
    KT_CHECK_INT_EQ(mfs_chgrp("/old.txt", 7), MFS_ENOTSUP);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);

    volume_teardown();
}

static void test_version_legacy_volume(void)
{
    struct mfs_inode inode;
    struct mfs_check_report report;

    ktest_begin("mfs format: a volume of version 1 is read and checked");

    KT_CHECK_INT_EQ(volume_setup(32), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/old.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mkdir("/old"), MFS_OK);
    mfs_unmount();

    /* The volume of 0.3: 16-byte records with no time and no group. */
    volume_to(MFS_VERSION_LEGACY);
    KT_CHECK_INT_EQ(g_sb.version, MFS_VERSION_LEGACY);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/old.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 0u);
    KT_CHECK_INT_EQ(inode.mtime, 0u);
    KT_CHECK_INT_EQ(inode.ctime, 0u);
    KT_CHECK_INT_EQ(mfs_inode_group(&inode), 0u);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MFS_MODE_DEFAULT);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.files, 1u);
    KT_CHECK_INT_EQ(report.directories, 2u);

    volume_teardown();
}

/*
 * The property of the record: the checksum of a version 3 record covers the
 * whole record but itself, so a byte of it that a torn write left behind is
 * a record the reader does not trust.
 */
static void test_record_checksum(void)
{
    /* The record is written into a raw span byte for byte, so the padding of
     * the struct is part of the record: a struct that is not zeroed answers
     * the checksum with bytes nobody wrote, which is what valgrind of the
     * cloud reported and a flaky checksum of a record would be. */
    struct mfs_inode inode;
    k_u8_t raw[FORMAT_RECORD];
    k_u32_t crc;

    ktest_begin("mfs format: the checksum of a record answers for the whole record");

    memset(raw, 0, sizeof(raw));
    memset(&inode, 0, sizeof(inode));
    inode.flags = MFS_INODE_USED;
    inode.first_block = 0xFFFFFFFFu;
    inode.size = 0;
    inode.entries = 0;
    inode.mtime = 0;
    inode.ctime = 0;
    inode.group = 3;
    memcpy(raw, &inode, sizeof(inode));

    crc = mfs_record_crc(raw);
    memcpy(raw + FORMAT_RECORD - sizeof(crc), &crc, sizeof(crc));
    KT_CHECK_INT_EQ(mfs_record_crc(raw), crc);

    /* Every field of the record is inside the span of the checksum. */
    {
        k_size_t fields[6];
        k_size_t i;

        fields[0] = offsetof(struct mfs_inode, flags);
        fields[1] = offsetof(struct mfs_inode, first_block);
        fields[2] = offsetof(struct mfs_inode, size);
        fields[3] = offsetof(struct mfs_inode, entries);
        fields[4] = offsetof(struct mfs_inode, mtime);
        fields[5] = offsetof(struct mfs_inode, group);

        for (i = 0; i < K_ARRAY_SIZE(fields); i++) {
            k_u8_t copy[FORMAT_RECORD];

            memcpy(copy, raw, sizeof(copy));
            copy[fields[i]] ^= 0x01u;
            KT_CHECK(mfs_record_crc(copy) != crc);
        }
    }

    /* And the padding and the reserved word are checked as well: a record
     * written by version 3 zeroes them and the format says so. */
    {
        k_u8_t copy[FORMAT_RECORD];
        k_u32_t zero = 0;

        memcpy(copy, raw, sizeof(copy));
        memcpy(copy + offsetof(struct mfs_inode_record, reserved), &zero, sizeof(zero));
        copy[FORMAT_RECORD - 8u] = 0x5Au;
        KT_CHECK(mfs_record_crc(copy) != crc);
    }
}

/*
 * The property of the growth: META is the records the volume was formatted
 * for, and a table that runs full grows into the data section instead of
 * refusing the object. The data of a file written before the growth is still
 * there after it, because a block of the tail is moved and not overwritten.
 */
static void test_table_grows(void)
{
    struct mfs_info before;
    struct mfs_info after;
    struct mfs_file file;
    struct mfs_check_report report;
    char path[32];
    k_u32_t i;
    k_u32_t records_before;
    k_u32_t small;
    k_u32_t big_blocks;

    ktest_begin("mfs format: the table of the records grows into the data section");

    KT_CHECK_INT_EQ(volume_setup(8), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);
    records_before = before.inodes_total;
    /* The records are the ones asked for, the reserve and the rounding of
     * the table to whole blocks of the device. */
    KT_CHECK_INT_EQ(records_before, ((8u + 64u + MFS_BLOCK_SIZE / MFS_INODE_SIZE - 1u) /
                                     (MFS_BLOCK_SIZE / MFS_INODE_SIZE)) *
                                            (MFS_BLOCK_SIZE / MFS_INODE_SIZE) -
                                        1u);

    /*
     * The volume is filled so that the blocks at the top of the data section
     * are blocks of a file when the table grows: the growth takes the four
     * blocks of the top, and it can only take them by moving their data into
     * free blocks below.
     *
     * The count of everything is the count of the volume: the root, the file
     * of the proof, one file of the rest of the data section, and a file per
     * record the table holds.
     */
    KT_CHECK_INT_EQ(mfs_create("/keep"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/keep", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "the bytes of the file stay", 26), 26);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    small = records_before - 3u; /* the root, /keep and /big */
    big_blocks = before.data_blocks - 2u - small;

    KT_CHECK_INT_EQ(mfs_create("/big"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/big", MFS_O_WRONLY, &file), MFS_OK);
    {
        /* A block of the chain holds MFS_BLOCK_SIZE - 4 bytes of the file. */
        k_u8_t chunk[MFS_BLOCK_SIZE - 4];
        k_u32_t at;

        memset(chunk, 0x5A, sizeof(chunk));

        for (at = 0; at < big_blocks; at++) {
            KT_CHECK_INT_EQ(mfs_write(&file, chunk, (k_size_t)sizeof(chunk)), (int)sizeof(chunk));
        }
    }
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* The records of the table, each with a byte of its own and one block,
     * fill the rest of the data section. */
    for (i = 0; i < small; i++) {
        k_u8_t byte = (k_u8_t)(i % 251u);

        snprintf(path, sizeof(path), "/a%03u", (unsigned)i);
        KT_CHECK_INT_EQ(mfs_create(path), MFS_OK);
        KT_CHECK_INT_EQ(mfs_open(path, MFS_O_WRONLY, &file), MFS_OK);
        KT_CHECK_INT_EQ(mfs_write(&file, &byte, sizeof(byte)), 1);
        KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    }

    /* Every record of the table is used now. A few of the files at the
     * bottom of the data section go, which leaves the holes the growth moves
     * the blocks of the top into. */
    for (i = 0; i < 8u; i++) {
        snprintf(path, sizeof(path), "/a%03u", (unsigned)i);
        KT_CHECK_INT_EQ(mfs_unlink(path), MFS_OK);
    }

    /* An empty file takes no block of the data section and one record: the
     * eight that the removal gave back, and then the one that needs more
     * records than the table holds. */
    for (i = 0; i < 8u; i++) {
        snprintf(path, sizeof(path), "/c%03u", (unsigned)i);
        KT_CHECK_INT_EQ(mfs_create(path), MFS_OK);
    }

    snprintf(path, sizeof(path), "/c%03u", 8u);
    KT_CHECK_INT_EQ(mfs_create(path), MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK(after.inodes_total > records_before);
    /* The growth is a whole number of blocks of the table, and a block holds
     * sixteen records of the format. */
    KT_CHECK((after.inodes_total + 1u) % (MFS_BLOCK_SIZE / MFS_INODE_SIZE) == 0u);
    /* The data section gave the blocks of the growth: META is larger. */
    KT_CHECK(after.meta_blocks > before.meta_blocks);
    KT_CHECK(after.data_blocks < before.data_blocks);

    /* The volume is one story and every byte of every file is where it was
     * before the growth moved the blocks of the tail. */
    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);

    KT_CHECK_INT_EQ(mfs_open("/keep", MFS_O_RDONLY, &file), MFS_OK);
    {
        char text[32];

        memset(text, 0, sizeof(text));
        KT_CHECK_INT_EQ(mfs_read(&file, text, 26), 26);
        KT_CHECK_STR_EQ(text, "the bytes of the file stay");
    }
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    for (i = 8u; i < small; i++) {
        k_u8_t byte = (k_u8_t)(i % 251u);
        char read_back = 0;

        snprintf(path, sizeof(path), "/a%03u", (unsigned)i);
        KT_CHECK_INT_EQ(mfs_open(path, MFS_O_RDONLY, &file), MFS_OK);
        KT_CHECK_INT_EQ(mfs_read(&file, &read_back, 1), 1);
        KT_CHECK_INT_EQ((k_u8_t)read_back, byte);
        KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    }

    /* The file of the data section, whole: every byte of it is the byte it
     * was, the blocks the growth moved included. */
    KT_CHECK_INT_EQ(mfs_open("/big", MFS_O_RDONLY, &file), MFS_OK);
    {
        k_u8_t chunk[MFS_BLOCK_SIZE - 4];
        k_u32_t at;
        k_u32_t wrong = 0;

        for (at = 0; at < big_blocks; at++) {
            k_size_t j;

            memset(chunk, 0, sizeof(chunk));
            KT_CHECK_INT_EQ(mfs_read(&file, chunk, (k_size_t)sizeof(chunk)), (int)sizeof(chunk));

            for (j = 0; j < sizeof(chunk); j++) {
                if (chunk[j] != 0x5Au) {
                    wrong++;
                }
            }
        }

        KT_CHECK_INT_EQ(wrong, 0u);
    }
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* The table of the grown volume is on the medium: a remount finds it. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK(after.inodes_total > records_before);
    KT_CHECK_INT_EQ(mfs_stat("/keep", NULL, NULL), MFS_OK);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);

    volume_teardown();
}

/*
 * The property of the descriptor: a growth of the table moves blocks of the
 * chains, and a chain that is open is only half on the medium - the first
 * block of a file that was written but not closed is named by the descriptor
 * and by no record. The growth therefore waits for the descriptors to close:
 * the create that needs more records than the table holds answers "no space"
 * while a file is open, and the file that is open stays whole (ADR 0038).
 */
static void test_open_across_growth(void)
{
    struct mfs_info before;
    struct mfs_info after;
    struct mfs_file open;
    struct mfs_file file;
    struct mfs_check_report report;
    char path[32];
    char text[64];
    k_u32_t i;
    int rc;

    ktest_begin("mfs format: the table waits for the files that are open");

    KT_CHECK_INT_EQ(volume_setup(8), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/open"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/open", MFS_O_RDWR, &open), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&open, "first", 5), 5);

    /* The table runs out of records with the file still open: the create
     * that needs the records after the last one is refused, and nothing of
     * the volume is touched. Two records are the root and the open file. */
    for (i = 0; i + 2u < before.inodes_total; i++) {
        snprintf(path, sizeof(path), "/n%03u", (unsigned)i);
        KT_CHECK_INT_EQ(mfs_create(path), MFS_OK);
    }

    snprintf(path, sizeof(path), "/n%03u", (unsigned)i);
    KT_CHECK_INT_EQ(mfs_create(path), MFS_ENOSPC);

    /* The descriptor writes the rest of its file after that. */
    KT_CHECK_INT_EQ(mfs_write(&open, "second", 6), 6);
    KT_CHECK_INT_EQ(mfs_close(&open), MFS_OK);

    /* The record of the file is on the medium and it is whole. */
    KT_CHECK_INT_EQ(mfs_open("/open", MFS_O_RDONLY, &file), MFS_OK);
    memset(text, 0, sizeof(text));
    KT_CHECK_INT_EQ(mfs_read(&file, text, 11), 11);
    KT_CHECK_STR_EQ(text, "firstsecond");
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);

    /* With no file open the table grows, and the volume of the grown table
     * is the same one after a restart. */
    rc = mfs_create(path);
    KT_CHECK_INT_EQ(rc, MFS_OK);

    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK(after.inodes_total > before.inodes_total);

    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/open", MFS_O_RDONLY, &file), MFS_OK);
    memset(text, 0, sizeof(text));
    KT_CHECK_INT_EQ(mfs_read(&file, text, 11), 11);
    KT_CHECK_STR_EQ(text, "firstsecond");
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);

    volume_teardown();
}

/*
 * The property of the checksum: a write that a power cut stopped in the
 * middle of the record is not half an object. The record says it is used and
 * does not checksum, so the check names it, the repair gives it back, and
 * the volume keeps everything it held before that write.
 */
static void test_torn_record(void)
{
    struct mfs_dirent dirent;
    struct mfs_file file;
    struct mfs_check_report report;
    k_u32_t block;
    k_u32_t offset;
    k_u32_t torn;

    ktest_begin("mfs format: a record left half written is named and repaired");

    KT_CHECK_INT_EQ(volume_setup(32), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/before.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/before.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "before", 6), 6);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    KT_CHECK_INT_EQ(mfs_create("/torn.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/torn.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "torn", 4), 4);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    memset(&dirent, 0, sizeof(dirent));
    KT_CHECK_INT_EQ(mfs_stat("/torn.txt", NULL, &dirent), MFS_OK);
    torn = dirent.inode;
    KT_CHECK(torn > MFS_ROOT_INODE);

    mfs_unmount();

    /*
     * The power cut: the block of the record reaches the medium with the
     * first word of the record and nothing of the rest, which is what a
     * write of 1 KB that stopped after four bytes leaves.
     */
    block = record_block(torn);
    offset = record_offset(torn);
    memset(volume_block(block) + offset + sizeof(k_u32_t), 0, FORMAT_RECORD - sizeof(k_u32_t));

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    /* The check names it and writes nothing. */
    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK(report.errors > 0);
    KT_CHECK(strstr(report.first, "checksum") != NULL);
    KT_CHECK_INT_EQ(report.fixed, 0);

    /* The repair gives the record back and the name of the object goes. */
    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_repair(&report), MFS_OK);
    KT_CHECK(report.fixed > 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.unfixed, 0);
    KT_CHECK_INT_EQ(mfs_stat("/torn.txt", NULL, NULL), MFS_ENOENT);

    /* The rest of the volume is the volume it was. */
    KT_CHECK_INT_EQ(mfs_stat("/before.txt", NULL, NULL), MFS_OK);
    {
        char text[8];

        KT_CHECK_INT_EQ(mfs_open("/before.txt", MFS_O_RDONLY, &file), MFS_OK);
        memset(text, 0, sizeof(text));
        KT_CHECK_INT_EQ(mfs_read(&file, text, sizeof(text)), 6);
        KT_CHECK_STR_EQ(text, "before");
        KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    }

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);

    /* And the volume is the same one after a restart. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/before.txt", NULL, NULL), MFS_OK);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_repair(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.fixed, 0);
    KT_CHECK_INT_EQ(report.errors, 0);

    volume_teardown();
}

static void test_torn_header(void)
{
    struct mfs_superblock damaged;
    struct mfs_check_report report;

    ktest_begin("mfs format: a header that does not checksum is not a volume");

    KT_CHECK_INT_EQ(volume_setup(32), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/file"), MFS_OK);
    mfs_unmount();

    /* The header of a version 3 volume covers the fields that place META. */
    memcpy(&damaged, volume_block(FORMAT_BLOCKS - 1u), sizeof(damaged));
    damaged.inode_records += 16u;
    memcpy(volume_block(FORMAT_BLOCKS - 1u), &damaged, sizeof(damaged));

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_EINVAL);

    /* The data of the volume is still there: a header is not a reason to
     * write anything, and the volume is refused instead. */
    memcpy(&damaged, volume_block(FORMAT_BLOCKS - 1u), sizeof(damaged));
    damaged.inode_records -= 16u;
    damaged.crc = test_crc32(&damaged, sizeof(damaged) - sizeof(damaged.crc));

    /* The checksum of version 3 is taken over the header with the field
     * empty, so the test writes it the way the format does. */
    {
        struct mfs_superblock copy = damaged;

        copy.crc = 0;
        damaged.crc = test_crc32(&copy, sizeof(copy));
    }

    memcpy(volume_block(FORMAT_BLOCKS - 1u), &damaged, sizeof(damaged));
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(mfs_stat("/file", NULL, NULL), MFS_OK);

    volume_teardown();
}

/*
 * The group: the record of a version 3 volume has a word for it, the number
 * survives a write of the object, and statx is where a program reads it
 * (ADR 0038). The door of the system calls that sets a group and the triad
 * of the group in the check of the access belong to another change.
 */
static void test_group(void)
{
    struct mfs_inode inode;
    struct mfs_file file;
    struct vfs_statx info;

    ktest_begin("mfs format: the group of an object is stored and read");

    KT_CHECK_INT_EQ(volume_setup(32), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/grouped"), MFS_OK);

    /* A new object is of the group of root, the way it is of its owner. */
    KT_CHECK_INT_EQ(mfs_stat("/grouped", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_group(&inode), 0u);

    KT_CHECK_INT_EQ(mfs_chgrp("/grouped", 42), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/grouped", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_group(&inode), 42u);

    /* A write of the object does not lose the group. */
    KT_CHECK_INT_EQ(mfs_open("/grouped", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "grouped", 7), 7);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/grouped", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_group(&inode), 42u);
    KT_CHECK_INT_EQ(inode.size, 7u);

    /* The adapter answers it through statx. */
    KT_CHECK_INT_EQ(mfs_vfs_register("/"), 0);
    memset(&info, 0, sizeof(info));
    KT_CHECK_INT_EQ(vfs_statx("/grouped", &info), MFS_OK);
    KT_CHECK_INT_EQ(info.group, 42u);
    KT_CHECK_INT_EQ(info.owner, 0u);
    KT_CHECK_INT_EQ(info.size, 7u);
    vfs_umount("/");

    /* The volume keeps it across a restart. */
    mfs_unmount();
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/grouped", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(mfs_inode_group(&inode), 42u);

    KT_CHECK_INT_EQ(mfs_chgrp("/nope", 1), MFS_ENOENT);

    volume_teardown();
}

void test_mfs_format(void)
{
    test_format_version();
    test_version_time_volume();
    test_version_legacy_volume();
    test_record_checksum();
    test_table_grows();
    test_open_across_growth();
    test_torn_record();
    test_torn_header();
    test_group();
}
