/*
 * The repair of MicroFS: a volume that a crash left inconsistent is put
 * right where the medium itself proves what the change has to be, and the
 * same volume without the flag is not touched at all.
 *
 * Every test here builds a sound volume, writes a file or a directory, and
 * then damages the medium the way an interrupted write would: an unmounted
 * volume is edited through the block device, so the mount and the check see
 * the bytes the board would see after a restart. The check has to name the
 * damage, the walk without the flag has to leave the bytes alone, and the
 * repair has to leave a volume that is 0 problems and that a second repair
 * does not change again.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdlib.h>
#include <string.h>

#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"

/* The size of the volume of the tests: 256 blocks of 1 KB. */
#define REPAIR_BLOCKS 256

/* The end of a chain of the data blocks, as the format writes it. */
#define REPAIR_NONE 0xFFFFFFFFu

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;
static struct mfs_superblock g_sb;

/* --- the volume --- */

static k_u8_t read_file_byte(const char *path, k_u32_t offset);

static void header_load(void)
{
    memcpy(&g_sb, g_volume.data + (size_t)(REPAIR_BLOCKS - 1) * MFS_BLOCK_SIZE, sizeof(g_sb));
}

static k_u32_t bitmap_blocks(void)
{
    return (g_sb.data_blocks + 8u * MFS_BLOCK_SIZE - 1u) / (8u * MFS_BLOCK_SIZE);
}

/* Where the map of the free blocks starts: the top of META in a version 3
 * volume, its start in the versions before it (ADR 0038). */
static k_u32_t bitmap_start(void)
{
    return (g_sb.version >= MFS_VERSION) ? g_sb.info_start - g_sb.bitmap_blocks : g_sb.meta_start;
}

/* The record of the mounted volume: 16 bytes for a volume of version 1. */
static k_u32_t record_size(void)
{
    if (g_sb.version == MFS_VERSION_LEGACY) {
        return MFS_INODE_SIZE_LEGACY;
    }

    return (g_sb.version == MFS_VERSION_TIME) ? MFS_INODE_SIZE_TIME : MFS_INODE_SIZE;
}

static k_u32_t inode_capacity(void)
{
    if (g_sb.version >= MFS_VERSION) {
        return g_sb.inode_records - 1u;
    }

    return (g_sb.meta_blocks - bitmap_blocks()) * MFS_BLOCK_SIZE / record_size() - 1u;
}

static k_u32_t inode_block(k_u32_t index)
{
    if (g_sb.version >= MFS_VERSION) {
        k_u32_t per_block = MFS_BLOCK_SIZE / record_size();
        k_u32_t slots = index + 1u;

        return (g_sb.info_start - g_sb.bitmap_blocks) - (slots + per_block - 1u) / per_block;
    }

    return g_sb.meta_start + bitmap_blocks() + (index * record_size()) / MFS_BLOCK_SIZE;
}

static k_u32_t inode_offset(k_u32_t index)
{
    if (g_sb.version >= MFS_VERSION) {
        k_u32_t per_block = MFS_BLOCK_SIZE / record_size();

        return ((per_block - (index + 1u) % per_block) % per_block) * record_size();
    }

    return (index * record_size()) % MFS_BLOCK_SIZE;
}

static int block_read(k_u32_t block, void *buf)
{
    return g_device.ops->read(g_device.ctx, block, buf, 1);
}

static int block_write(k_u32_t block, const void *buf)
{
    return g_device.ops->write(g_device.ctx, block, buf, 1);
}

/* A record of the table, through the device, the way a writer of it works. */
static k_u32_t legacy_crc32(const void *data, k_size_t size);

/* The bytes of a record that are its fields: the record of a version 3
 * volume is longer than the structure and carries a checksum after them. */
static k_size_t record_fields(void)
{
    return (record_size() < sizeof(struct mfs_inode)) ? record_size() : sizeof(struct mfs_inode);
}

static int inode_get(k_u32_t index, struct mfs_inode *out)
{
    k_u8_t block[MFS_BLOCK_SIZE];

    if (block_read(inode_block(index), block) != 0) {
        return -1;
    }

    /* The reader of the filesystem answers zero for the times of a record
     * that has no room for them, and so does this one. */
    memset(out, 0, sizeof(*out));
    memcpy(out, block + inode_offset(index), record_fields());

    return 0;
}

static int inode_put(k_u32_t index, const struct mfs_inode *inode)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t at = inode_offset(index);

    if (block_read(inode_block(index), block) != 0) {
        return -1;
    }

    memset(block + at, 0, record_size());
    memcpy(block + at, inode, record_fields());

    /* A record of a version 3 volume carries its checksum; a record without
     * one is the torn write the check of the volume names (ADR 0038). */
    if (record_size() == MFS_INODE_SIZE) {
        k_u32_t crc = legacy_crc32(block + at, MFS_INODE_SIZE - sizeof(k_u32_t));

        memcpy(block + at + MFS_INODE_SIZE - sizeof(k_u32_t), &crc, sizeof(crc));
    }

    return block_write(inode_block(index), block);
}

/* The first record of the table that is free, 0 when there is none. */
static k_u32_t free_record(void)
{
    k_u32_t index;

    for (index = 2; index <= inode_capacity(); index++) {
        struct mfs_inode inode;

        if (inode_get(index, &inode) == 0 && (inode.flags & MFS_INODE_USED) == 0u &&
            inode.flags == 0u) {
            return index;
        }
    }

    return 0;
}

/* The block that follows `block` in its chain. */
static int chain_next(k_u32_t block, k_u32_t *next)
{
    k_u8_t data[MFS_BLOCK_SIZE];

    if (block_read(block, data) != 0) {
        return -1;
    }

    memcpy(next, data, sizeof(*next));

    return 0;
}

/* A bit of the map of the free blocks, through the device. The map of a
 * version 3 volume is at the top of META and the one of the versions before
 * it at its start (ADR 0038). */
static int bitmap_get(k_u32_t block)
{
    k_u8_t data[MFS_BLOCK_SIZE];

    if (block_read(bitmap_start(), data) != 0) {
        return -1;
    }

    return (data[block / 8u] & (1u << (block % 8u))) != 0u;
}

static int bitmap_put(k_u32_t block, bool used)
{
    k_u8_t data[MFS_BLOCK_SIZE];

    if (block_read(bitmap_start(), data) != 0) {
        return -1;
    }

    if (used) {
        data[block / 8u] |= (k_u8_t)(1u << (block % 8u));
    } else {
        data[block / 8u] &= (k_u8_t) ~(1u << (block % 8u));
    }

    return block_write(bitmap_start(), data);
}

/* The first block of the data section the map calls free. */
static k_u32_t free_block(void)
{
    k_u32_t block;

    for (block = 1; block < g_sb.data_blocks; block++) {
        if (bitmap_get(block) == 0) {
            return block;
        }
    }

    return REPAIR_NONE;
}

/*
 * The offset of an entry of a one-block directory, from the start of the
 * block: the entries begin after the four bytes of the chain.
 */
static k_size_t entry_find(k_u32_t block, const char *name)
{
    k_u8_t data[MFS_BLOCK_SIZE];
    k_size_t offset = 0;
    k_size_t len = strlen(name);

    if (block_read(block, data) != 0) {
        return (k_size_t)-1;
    }

    while (4u + offset + sizeof(struct mfs_dirent_header) <= MFS_BLOCK_SIZE) {
        k_u16_t name_len;

        memcpy(&name_len, data + 4u + offset + 4u, sizeof(name_len));

        if ((k_size_t)name_len == len &&
            memcmp(data + 4u + offset + sizeof(struct mfs_dirent_header), name, len) == 0) {
            return 4u + offset;
        }

        if (name_len == 0u ||
            4u + offset + sizeof(struct mfs_dirent_header) + name_len > MFS_BLOCK_SIZE) {
            break;
        }

        offset += sizeof(struct mfs_dirent_header) + name_len;
    }

    return (k_size_t)-1;
}

static int setup(void)
{
    int rc = mfs_ram_init(&g_volume, REPAIR_BLOCKS);

    if (rc != 0) {
        return rc;
    }

    mfs_ram_device(&g_volume, &g_device);

    rc = mfs_format(&g_device, 64);
    header_load();

    return rc;
}

static void teardown(void)
{
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static bool write_file(const char *path, char fill, k_size_t size)
{
    static k_u8_t data[4096];
    struct mfs_file file;

    memset(data, fill, sizeof(data));

    if (mfs_create(path) != MFS_OK || mfs_open(path, MFS_O_WRONLY, &file) != MFS_OK) {
        return false;
    }

    if (mfs_write(&file, data, size) != (int)size) {
        return false;
    }

    return mfs_close(&file) == MFS_OK;
}

/* The bytes of the volume, for the test that nothing may be written. */
static k_u8_t *volume_copy(void)
{
    k_u8_t *copy = (k_u8_t *)malloc((size_t)REPAIR_BLOCKS * MFS_BLOCK_SIZE);

    if (copy != NULL) {
        memcpy(copy, g_volume.data, (size_t)REPAIR_BLOCKS * MFS_BLOCK_SIZE);
    }

    return copy;
}

static bool volume_same(const k_u8_t *copy)
{
    return copy != NULL && memcmp(copy, g_volume.data, (size_t)REPAIR_BLOCKS * MFS_BLOCK_SIZE) == 0;
}

/*
 * The walk that is only a report: it names the damage, it writes nothing, and
 * its counters are the ones of the volume as it was found.
 */
static void expect_report_only(const char *worth)
{
    struct mfs_check_report report;
    k_u8_t *before = volume_copy();

    KT_CHECK(before != NULL);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK(report.errors > 0);
    KT_CHECK(report.first[0] != '\0');
    KT_CHECK(strstr(report.first, worth) != NULL);
    KT_CHECK_INT_EQ(report.fixed, 0);

    /* The check reads and never writes: the medium is the same byte by byte. */
    KT_CHECK(volume_same(before));

    free(before);
}

/* The repair: the damage is gone, the pass wrote what it promised, and a
 * second pass has nothing left to do. */
static void expect_repaired(void)
{
    struct mfs_check_report report;

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_repair(&report), MFS_OK);
    KT_CHECK(report.fixed > 0);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.unfixed, 0);
    KT_CHECK_INT_EQ(report.first[0], '\0');

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_repair(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.fixed, 0);
    KT_CHECK_INT_EQ(report.errors, 0);
}

/*
 * Turn the volume of the test into the version 1 that came before the
 * times: the same records in the 16 bytes they had. The records are read
 * into the memory first, because the smaller record moves over the bytes of
 * the larger one.
 */
#define LEGACY_RECORDS 256

static k_u32_t legacy_crc32(const void *data, k_size_t size)
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

static void volume_to_legacy(void)
{
    struct mfs_inode records[LEGACY_RECORDS];
    k_u8_t map[MFS_BLOCK_SIZE];
    k_u8_t *table;
    k_u32_t capacity = inode_capacity();
    k_u32_t i;

    if (capacity >= LEGACY_RECORDS) {
        capacity = LEGACY_RECORDS - 1u;
    }

    memset(records, 0, sizeof(records));

    for (i = 1; i <= capacity; i++) {
        KT_CHECK_INT_EQ(inode_get(i, &records[i]), 0);
    }

    /* The map moves from the top of META to its start, the old place of the
     * table (ADR 0038), and the table of the smaller records follows it. */
    memcpy(map, g_volume.data + (size_t)bitmap_start() * MFS_BLOCK_SIZE, MFS_BLOCK_SIZE);

    memset(g_volume.data + (size_t)g_sb.meta_start * MFS_BLOCK_SIZE, 0,
           (size_t)g_sb.meta_blocks * MFS_BLOCK_SIZE);
    memcpy(g_volume.data + (size_t)g_sb.meta_start * MFS_BLOCK_SIZE, map, MFS_BLOCK_SIZE);

    table = g_volume.data + (size_t)(g_sb.meta_start + bitmap_blocks()) * MFS_BLOCK_SIZE;

    for (i = 1; i <= capacity; i++) {
        if ((records[i].flags & MFS_INODE_USED) != 0u) {
            memcpy(table + (size_t)i * MFS_INODE_SIZE_LEGACY, &records[i], MFS_INODE_SIZE_LEGACY);
        }
    }

    g_sb.version = MFS_VERSION_LEGACY;
    g_sb.features &= (k_u32_t) ~(MFS_FEATURE_TIME | MFS_FEATURE_RECORD_CRC);
    g_sb.bitmap_blocks = 0;
    g_sb.inode_blocks = 0;
    g_sb.inode_size = 0;
    g_sb.inode_records = 0;
    g_sb.crc = legacy_crc32(&g_sb, MFS_HEADER_CRC_SPAN);

    memcpy(g_volume.data + (size_t)(REPAIR_BLOCKS - 1) * MFS_BLOCK_SIZE, &g_sb, sizeof(g_sb));
}

/* --- the damage and its repair --- */

/*
 * A block that the map of the free blocks calls used and that no object
 * holds is space no file will ever get back: the next writer only looks at
 * the zeros of the map. It goes back to the map.
 */
static void test_orphan_block(void)
{
    struct mfs_info before;
    struct mfs_info after;
    k_u32_t block;

    ktest_begin("mfs repair: an orphan block goes back to the free map");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_mkdir("/logs") == MFS_OK && write_file("/logs/a", 'a', 3000));
    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);
    mfs_unmount();

    block = free_block();
    KT_CHECK(block != REPAIR_NONE);
    KT_CHECK_INT_EQ(bitmap_put(block, true), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("is marked used but no object owns it");

    expect_repaired();

    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.data_free, before.data_free);
    KT_CHECK_INT_EQ(bitmap_get(block), 0);

    teardown();
}

/*
 * An entry of a directory that names a free record is a name with nothing
 * behind it: no open, no stat can answer it. The entry goes, and the record
 * it named - if it was a record nobody else reaches - with it.
 */
static void test_entry_to_free_record(void)
{
    struct mfs_inode dir;
    struct mfs_dirent entry;
    k_u32_t dir_block;
    k_u32_t hole;
    k_u32_t freed;
    k_size_t at;
    k_u8_t block[MFS_BLOCK_SIZE];

    ktest_begin("mfs repair: an entry that names a free record is removed");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_mkdir("/logs") == MFS_OK && write_file("/logs/a", 'a', 100));
    KT_CHECK_INT_EQ(mfs_stat("/logs", &dir, &entry), MFS_OK);
    dir_block = dir.first_block;
    KT_CHECK_INT_EQ(mfs_stat("/logs/a", NULL, &entry), MFS_OK);
    freed = entry.inode;
    mfs_unmount();

    hole = free_record();
    KT_CHECK(hole != 0);
    at = entry_find(dir_block, "a");
    KT_CHECK(at != (k_size_t)-1);

    KT_CHECK_INT_EQ(block_read(dir_block, block), 0);
    memcpy(block + at, &hole, sizeof(hole));
    KT_CHECK_INT_EQ(block_write(dir_block, block), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("an entry points at a free record");

    expect_repaired();

    /* The name cannot be reached any more, and its record is free again. */
    KT_CHECK_INT_EQ(mfs_stat("/logs/a", NULL, NULL), MFS_ENOENT);
    {
        struct mfs_inode record;

        KT_CHECK_INT_EQ(inode_get(freed, &record), 0);
        KT_CHECK_INT_EQ(record.flags, 0u);
    }

    teardown();
}

/*
 * An entry that names a record the table does not hold - the number a
 * half-written write of an entry left - has nothing behind it either, and
 * goes the same way.
 */
static void test_entry_outside_table(void)
{
    struct mfs_inode dir;
    k_u32_t dir_block;
    k_u32_t outside;
    k_size_t at;
    k_u8_t block[MFS_BLOCK_SIZE];

    ktest_begin("mfs repair: an entry outside the table is removed");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_mkdir("/logs") == MFS_OK && write_file("/logs/a", 'a', 100));
    KT_CHECK_INT_EQ(mfs_stat("/logs", &dir, NULL), MFS_OK);
    dir_block = dir.first_block;
    mfs_unmount();

    outside = inode_capacity() + 1u;
    at = entry_find(dir_block, "a");
    KT_CHECK(at != (k_size_t)-1);

    KT_CHECK_INT_EQ(block_read(dir_block, block), 0);
    memcpy(block + at, &outside, sizeof(outside));
    KT_CHECK_INT_EQ(block_write(dir_block, block), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("an entry points outside the table");

    expect_repaired();

    KT_CHECK_INT_EQ(mfs_stat("/logs/a", NULL, NULL), MFS_ENOENT);

    teardown();
}

/*
 * Two entries of one directory that name one record are one name too many:
 * this format has no second name of a file, so the later entry is removed and
 * the record keeps the data of the name that came first.
 */
static void test_entry_twice(void)
{
    struct mfs_inode dir;
    struct mfs_inode record;
    struct mfs_dirent entry;
    k_u32_t dir_block;
    k_u32_t first_index;
    k_u32_t second_index;
    k_size_t at;
    k_u8_t block[MFS_BLOCK_SIZE];

    ktest_begin("mfs repair: the second entry of one record is removed");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_mkdir("/logs") == MFS_OK && write_file("/logs/a", 'a', 100) &&
             write_file("/logs/b", 'b', 100));
    KT_CHECK_INT_EQ(mfs_stat("/logs", &dir, NULL), MFS_OK);
    dir_block = dir.first_block;
    KT_CHECK_INT_EQ(mfs_stat("/logs/a", NULL, &entry), MFS_OK);
    first_index = entry.inode;
    KT_CHECK_INT_EQ(mfs_stat("/logs/b", NULL, &entry), MFS_OK);
    second_index = entry.inode;
    mfs_unmount();

    /* The entry of "b" names the record of "a": the record of "b" is not
     * named any more, and the walk of the volume sees neither damage. */
    at = entry_find(dir_block, "b");
    KT_CHECK(at != (k_size_t)-1);
    KT_CHECK_INT_EQ(block_read(dir_block, block), 0);
    memcpy(block + at, &first_index, sizeof(first_index));
    KT_CHECK_INT_EQ(block_write(dir_block, block), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    {
        struct mfs_check_report report;
        k_u8_t *before = volume_copy();

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK(volume_same(before));
        free(before);
    }

    expect_repaired();

    /* The first name keeps the data, the second is gone, and the record it
     * had is free again. */
    KT_CHECK_INT_EQ(mfs_stat("/logs/a", NULL, NULL), MFS_OK);
    KT_CHECK_INT_EQ(read_file_byte("/logs/a", 0), 'a');
    KT_CHECK_INT_EQ(mfs_stat("/logs/b", NULL, NULL), MFS_ENOENT);
    KT_CHECK_INT_EQ(inode_get(second_index, &record), 0);
    KT_CHECK_INT_EQ(record.flags, 0u);

    teardown();
}

/*
 * A size that promises more bytes than the chain holds is cut down to what
 * the medium really has; a chain that runs past the size of the file gives
 * the blocks beyond it back.
 */
static void test_size_against_chain(void)
{
    struct mfs_info before;
    struct mfs_info after;
    struct mfs_inode inode;
    struct mfs_dirent entry;
    k_u32_t index;
    k_u32_t block;
    k_u32_t tail = 0;

    ktest_begin("mfs repair: a size that does not match its chain is put right");

    /* The size of a file that is larger than its chain. */
    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(write_file("/big", 'b', 2000));
    KT_CHECK_INT_EQ(mfs_stat("/big", &inode, &entry), MFS_OK);
    index = entry.inode;
    mfs_unmount();

    KT_CHECK_INT_EQ(inode_get(index, &inode), 0);
    inode.size = 3000;
    KT_CHECK_INT_EQ(inode_put(index, &inode), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("the chain is shorter than the file");

    expect_repaired();

    KT_CHECK_INT_EQ(mfs_stat("/big", &inode, NULL), MFS_OK);
    /* Two blocks of 1020 bytes of data: the file cannot be longer. */
    KT_CHECK_INT_EQ(inode.size, 2u * (MFS_BLOCK_SIZE - 4u));
    teardown();

    /* The chain of a file that is longer than its size. */
    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(write_file("/long", 'l', 2000));
    KT_CHECK_INT_EQ(mfs_stat("/long", &inode, &entry), MFS_OK);
    index = entry.inode;
    block = inode.first_block;
    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);
    KT_CHECK_INT_EQ(chain_next(block, &tail), 0);
    KT_CHECK(tail != REPAIR_NONE);
    mfs_unmount();

    KT_CHECK_INT_EQ(inode_get(index, &inode), 0);
    inode.size = 1000;
    KT_CHECK_INT_EQ(inode_put(index, &inode), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("the file keeps blocks beyond its end");

    expect_repaired();

    KT_CHECK_INT_EQ(mfs_stat("/long", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 1000u);
    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.data_free, before.data_free + 1u);

    /* The first block is still the file's; the tail is free again. */
    KT_CHECK_INT_EQ(bitmap_get(block), 1);
    KT_CHECK_INT_EQ(bitmap_get(tail), 0);

    teardown();
}

/*
 * Two objects that name one block share it: the first owner of the walk of
 * the tree keeps it, which is the name whose entry comes first, and the
 * second becomes an empty file with its name still there - the rule of FAT.
 */
static void test_shared_block(void)
{
    struct mfs_inode first;
    struct mfs_inode second;
    struct mfs_dirent entry;
    k_u32_t second_index;
    char text[64];

    ktest_begin("mfs repair: the second of two objects gives a shared block up");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(write_file("/first", '1', 1000));
    KT_CHECK(write_file("/second", '2', 1000));
    KT_CHECK_INT_EQ(mfs_stat("/second", &second, &entry), MFS_OK);
    second_index = entry.inode;
    KT_CHECK_INT_EQ(mfs_stat("/first", &first, NULL), MFS_OK);
    mfs_unmount();

    /* The second file names the block of the first one. */
    second.first_block = first.first_block;
    KT_CHECK_INT_EQ(inode_put(second_index, &second), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("belongs to more than one object");

    expect_repaired();

    KT_CHECK_INT_EQ(mfs_stat("/first", &first, NULL), MFS_OK);
    KT_CHECK_INT_EQ(first.size, 1000u);
    KT_CHECK_INT_EQ(mfs_stat("/second", &second, NULL), MFS_OK);
    KT_CHECK_INT_EQ(second.size, 0u);
    KT_CHECK_INT_EQ(second.first_block, REPAIR_NONE);

    /* The data of the name that came first is still the data it was. */
    {
        struct mfs_file file;

        KT_CHECK_INT_EQ(mfs_open("/first", MFS_O_RDONLY, &file), MFS_OK);
        KT_CHECK_INT_EQ(mfs_read(&file, text, 4), 4);
        KT_CHECK_INT_EQ(memcmp(text, "1111", 4), 0);
        KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    }

    teardown();
}

/* A chain that ends in itself is a loop; it is cut at the block that repeats,
 * so the file keeps the data that was its own. */
static void test_loop(void)
{
    struct mfs_inode inode;
    struct mfs_dirent entry;
    k_u32_t block;
    k_u8_t data[MFS_BLOCK_SIZE];

    ktest_begin("mfs repair: a chain that loops is cut");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(write_file("/loop", 'p', 1000));
    KT_CHECK_INT_EQ(mfs_stat("/loop", &inode, &entry), MFS_OK);
    block = inode.first_block;
    mfs_unmount();

    KT_CHECK_INT_EQ(block_read(block, data), 0);
    memcpy(data, &block, sizeof(block));
    KT_CHECK_INT_EQ(block_write(block, data), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("belongs to more than one object");

    expect_repaired();

    KT_CHECK_INT_EQ(block_read(block, data), 0);
    {
        k_u32_t next;

        memcpy(&next, data, sizeof(next));
        KT_CHECK_INT_EQ(next, REPAIR_NONE);
    }

    KT_CHECK_INT_EQ(mfs_stat("/loop", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 1000u);
    KT_CHECK_INT_EQ(read_file_byte("/loop", 0), 'p');

    teardown();
}

/* The counter of the entries of a directory is a copy of what its bytes say:
 * it is counted again and written back. */
static void test_entries_counter(void)
{
    struct mfs_inode inode;
    struct mfs_dirent entry;
    k_u32_t index;

    ktest_begin("mfs repair: the counter of a directory is counted again");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_mkdir("/logs") == MFS_OK && write_file("/logs/a", 'a', 10) &&
             write_file("/logs/b", 'b', 10));
    KT_CHECK_INT_EQ(mfs_stat("/logs", &inode, &entry), MFS_OK);
    index = entry.inode;
    mfs_unmount();

    KT_CHECK_INT_EQ(inode_get(index, &inode), 0);
    inode.entries += 5u;
    KT_CHECK_INT_EQ(inode_put(index, &inode), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("the counter of the entries does not match the entries");

    expect_repaired();

    KT_CHECK_INT_EQ(mfs_stat("/logs", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.entries, 2u);

    teardown();
}

/*
 * A record that no name reaches is what a create or an unlink leaves when it
 * stops between the record and the entry: the name never reached the medium,
 * so the record and its block are space no path can find again. The report
 * of the check does not see it - nothing walks the table against the tree -
 * and the repair frees it.
 */
static void test_orphan_record(void)
{
    struct mfs_info before;
    struct mfs_info after;
    struct mfs_inode orphan;
    k_u32_t index;
    k_u32_t block;

    ktest_begin("mfs repair: a record no name reaches is freed");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(write_file("/a", 'a', 10));
    KT_CHECK_INT_EQ(mfs_get_info(&before), MFS_OK);
    mfs_unmount();

    index = free_record();
    block = free_block();
    KT_CHECK(index != 0 && block != REPAIR_NONE);
    KT_CHECK_INT_EQ(bitmap_put(block, true), 0);

    /* The state of a mkdir that stopped after the block was cleared and the
     * record was written, and before the entry named it. */
    {
        k_u8_t empty[MFS_BLOCK_SIZE];

        memset(empty, 0, sizeof(empty));
        memcpy(empty, &(k_u32_t){REPAIR_NONE}, sizeof(k_u32_t));
        KT_CHECK_INT_EQ(block_write(block, empty), 0);
    }

    memset(&orphan, 0, sizeof(orphan));
    orphan.flags =
        MFS_INODE_USED | MFS_INODE_DIR | ((k_u32_t)MFS_MODE_DIR_DEFAULT << MFS_MODE_SHIFT);
    orphan.first_block = block;
    orphan.size = 0;
    orphan.entries = 0;
    KT_CHECK_INT_EQ(inode_put(index, &orphan), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    /* The walk of the volume has nothing to name: the record is invisible. */
    {
        struct mfs_check_report report;
        k_u8_t *before_bytes = volume_copy();

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK_INT_EQ(report.errors, 0);
        KT_CHECK(volume_same(before_bytes));
        free(before_bytes);
    }

    expect_repaired();

    KT_CHECK_INT_EQ(mfs_get_info(&after), MFS_OK);
    KT_CHECK_INT_EQ(after.inodes_used, before.inodes_used);
    KT_CHECK_INT_EQ(after.data_free, before.data_free);
    KT_CHECK_INT_EQ(bitmap_get(block), 0);

    teardown();
}

/* The bits between the type of a record and its mode belong to nobody, and a
 * free record that carries flags is the wake of an interrupted write. */
static void test_flags(void)
{
    struct mfs_inode inode;
    struct mfs_dirent entry;
    k_u32_t index;
    k_u32_t hole;

    ktest_begin("mfs repair: the flags of a record are put right");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(write_file("/f", 'f', 10));
    KT_CHECK_INT_EQ(mfs_stat("/f", &inode, &entry), MFS_OK);
    index = entry.inode;
    mfs_unmount();

    KT_CHECK_INT_EQ(inode_get(index, &inode), 0);
    inode.flags |= 0x04u;
    KT_CHECK_INT_EQ(inode_put(index, &inode), 0);

    hole = free_record();
    KT_CHECK(hole != 0);
    memset(&inode, 0, sizeof(inode));
    inode.flags = 0x04u;
    KT_CHECK_INT_EQ(inode_put(hole, &inode), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("reserved bits of the flags");

    expect_repaired();

    KT_CHECK_INT_EQ(mfs_stat("/f", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.flags & 0x04u, 0u);
    KT_CHECK_INT_EQ(mfs_inode_mode(&inode), MFS_MODE_DEFAULT);
    KT_CHECK_INT_EQ(inode_get(hole, &inode), 0);
    KT_CHECK_INT_EQ(inode.flags, 0u);

    teardown();
}

/*
 * The header of the volume is not repaired: a volume whose header is not the
 * one of the design is a volume this system does not mount, and a header that
 * disagrees with the mount means the medium changed under it. The pass writes
 * nothing and says why.
 */
static void test_header_refused(void)
{
    struct mfs_check_report report;
    k_u8_t *before;

    ktest_begin("mfs repair: a damaged header is not repaired");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(write_file("/a", 'a', 10));

    /* The header is edited under the mount: the check reads it again. */
    g_volume.data[(size_t)(REPAIR_BLOCKS - 1) * MFS_BLOCK_SIZE + 40] ^= 0x01u;

    before = volume_copy();
    KT_CHECK(before != NULL);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_repair(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.fixed, 0);
    KT_CHECK(report.errors > 0);
    KT_CHECK(report.unfixed > 0);
    KT_CHECK(strncmp(report.first, "header: ", 8) == 0);
    KT_CHECK(volume_same(before));

    free(before);

    /* The data of the volume is still there for a person to copy off. */
    KT_CHECK_INT_EQ(mfs_stat("/a", NULL, NULL), MFS_OK);

    teardown();
}

/*
 * One volume, every kind of damage at once, and the rule that a report is a
 * report: the bytes are the same before and after the walk without the flag,
 * and the volume is whole after the repair.
 */
static void test_mixed_damage(void)
{
    struct mfs_inode inode;
    struct mfs_dirent entry;
    k_u32_t index;
    k_u32_t block;
    k_u8_t *before;

    ktest_begin("mfs repair: many problems at once are all put right");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_mkdir("/logs") == MFS_OK && write_file("/logs/a", 'a', 2000) &&
             write_file("/b", 'b', 1000));
    KT_CHECK_INT_EQ(mfs_stat("/b", &inode, &entry), MFS_OK);
    index = entry.inode;
    mfs_unmount();

    /* A block nobody owns, a size too big, and a loop. */
    block = free_block();
    KT_CHECK(block != REPAIR_NONE);
    KT_CHECK_INT_EQ(bitmap_put(block, true), 0);

    KT_CHECK_INT_EQ(inode_get(index, &inode), 0);
    inode.size = 5000;
    KT_CHECK_INT_EQ(inode_put(index, &inode), 0);

    {
        struct mfs_inode logs;
        k_u8_t data[MFS_BLOCK_SIZE];
        k_u32_t self;

        KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
        KT_CHECK_INT_EQ(mfs_stat("/logs/a", &logs, NULL), MFS_OK);
        self = logs.first_block;
        mfs_unmount();

        KT_CHECK_INT_EQ(block_read(self, data), 0);
        memcpy(data, &self, sizeof(self));
        KT_CHECK_INT_EQ(block_write(self, data), 0);
    }

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    before = volume_copy();
    KT_CHECK(before != NULL);
    {
        struct mfs_check_report report;

        memset(&report, 0, sizeof(report));
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK(report.errors > 0);
    }
    KT_CHECK(volume_same(before));
    free(before);

    expect_repaired();

    /* Every name still answers, and /logs/a keeps the block of its data. */
    KT_CHECK_INT_EQ(mfs_stat("/b", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, MFS_BLOCK_SIZE - 4u);
    KT_CHECK_INT_EQ(mfs_stat("/logs/a", NULL, NULL), MFS_OK);
    KT_CHECK_INT_EQ(read_file_byte("/logs/a", 0), 'a');

    teardown();
}

/*
 * A volume of version 1 has 16-byte records: the repair writes the 16 bytes
 * the record has and never the eight of the time that follows it, so the
 * record next to the one it puts right stays whole.
 */
static void test_legacy_volume(void)
{
    struct mfs_inode inode;
    struct mfs_inode neighbour;
    struct mfs_dirent entry;
    k_u32_t index;
    k_u8_t byte;

    ktest_begin("mfs repair: the repair of a legacy volume writes its records");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(write_file("/a", 'a', 100));
    KT_CHECK(write_file("/b", 'b', 100));
    KT_CHECK_INT_EQ(mfs_stat("/a", &inode, &entry), MFS_OK);
    index = entry.inode;
    mfs_unmount();

    volume_to_legacy();
    header_load();

    KT_CHECK_INT_EQ(inode_get(index, &inode), 0);
    KT_CHECK_INT_EQ(inode_get(index + 1u, &neighbour), 0);
    inode.size = 5000;
    KT_CHECK_INT_EQ(inode_put(index, &inode), 0);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    expect_report_only("the chain is shorter than the file");

    expect_repaired();

    /* The record next door is the one it was: a write of 32 bytes would
     * have put the zero times of /a over its flags. */
    KT_CHECK_INT_EQ(inode_get(index + 1u, &inode), 0);
    KT_CHECK_INT_EQ(inode.flags, neighbour.flags);
    KT_CHECK_INT_EQ(inode.first_block, neighbour.first_block);
    KT_CHECK_INT_EQ(inode.size, neighbour.size);

    KT_CHECK_INT_EQ(mfs_stat("/a", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, MFS_BLOCK_SIZE - 4u);
    KT_CHECK_INT_EQ(mfs_stat("/b", NULL, NULL), MFS_OK);
    byte = read_file_byte("/b", 0);
    KT_CHECK_INT_EQ(byte, 'b');

    teardown();
}

/* A sound volume is not touched by the flag either: there is nothing to do. */
static void test_sound_volume(void)
{
    struct mfs_check_report report;
    k_u8_t *before;

    ktest_begin("mfs repair: a sound volume is not written");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK(mfs_mkdir("/logs") == MFS_OK && write_file("/logs/a", 'a', 3000));

    before = volume_copy();
    KT_CHECK(before != NULL);

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_repair(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
    KT_CHECK_INT_EQ(report.fixed, 0);
    KT_CHECK(volume_same(before));

    free(before);

    teardown();
}

/*
 * The property: the repair is not limited to a table of 32768 records. A
 * volume with 40000 of them repairs an orphan record beyond that number, so
 * the map of the walk is the volume's and not a fixed table.
 */
static void test_big_table(void)
{
    enum { BIG_BLOCKS = 4096 };
    struct mfs_check_report report;
    struct mfs_superblock sb;
    struct mfs_ram_device volume;
    struct k_blockdev device;
    struct mfs_info info;
    struct mfs_inode inode;
    struct mfs_inode after;
    k_u32_t index = 33000u;
    int rc;

    ktest_begin("mfs: the repair of a table of records beyond 32768");

    KT_CHECK_INT_EQ(mfs_ram_init(&volume, BIG_BLOCKS), 0);
    mfs_ram_device(&volume, &device);
    KT_CHECK_INT_EQ(mfs_format(&device, 40000), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_get_info(&info), MFS_OK);
    KT_CHECK(info.inodes_total > index);

    memcpy(&sb, volume.data + (size_t)(BIG_BLOCKS - 1) * MFS_BLOCK_SIZE, sizeof(sb));
    /* The helpers of the file read the volume through the globals of the
     * fixture; this test has a volume of its own. */
    g_device = device;
    g_sb = sb;

    /*
     * A record that is used and that no name reaches, beyond the 32768 the
     * map of the records of 1.4 held. It goes through the device with the
     * checksum of the format, so that what the repair meets is a record
     * nobody reaches and not a record that a write tore (ADR 0038).
     */
    memset(&inode, 0, sizeof(inode));
    inode.flags = MFS_INODE_USED;
    KT_CHECK_INT_EQ(inode_put(index, &inode), 0);

    rc = mfs_repair(&report);
    KT_CHECK_INT_EQ(rc, MFS_OK);
    KT_CHECK(report.errors == 0);
    KT_CHECK(report.fixed > 0);

    memset(&after, 0, sizeof(after));
    KT_CHECK_INT_EQ(inode_get(index, &after), 0);
    KT_CHECK_INT_EQ(after.flags, 0);

    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);

    mfs_unmount();
    mfs_ram_free(&volume);
}

/* --- the fixture --- */

static k_u8_t read_file_byte(const char *path, k_u32_t offset)
{
    struct mfs_file file;
    k_u8_t byte = 0;

    if (mfs_open(path, MFS_O_RDONLY, &file) != MFS_OK) {
        return 0;
    }

    if (offset != 0u) {
        k_u8_t skip[256];

        while (offset > 0u) {
            k_size_t chunk = (offset > sizeof(skip)) ? sizeof(skip) : offset;

            if (mfs_read(&file, skip, chunk) != (int)chunk) {
                mfs_close(&file);
                return 0;
            }

            offset -= (k_u32_t)chunk;
        }
    }

    (void)mfs_read(&file, &byte, 1);
    mfs_close(&file);

    return byte;
}

void test_mfs_repair(void)
{
    test_orphan_block();
    test_entry_to_free_record();
    test_entry_outside_table();
    test_entry_twice();
    test_size_against_chain();
    test_shared_block();
    test_loop();
    test_entries_counter();
    test_orphan_record();
    test_flags();
    test_header_refused();
    test_mixed_damage();
    test_legacy_volume();
    test_sound_volume();
    test_big_table();
}
