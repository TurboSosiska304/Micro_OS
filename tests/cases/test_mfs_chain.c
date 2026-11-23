/*
 * Micro OS — the cost of reading a file of many blocks.
 *
 * A chain of MicroFS is a list of blocks: a step to the next block is a read
 * of the header of a block of the device, and a walk from the head is the
 * only way to a block. A read that walks from the head for every block of a
 * file therefore costs the square of the number of the blocks - the port of
 * zstd showed it as seconds between `exec /bin/zstd` and its first line.
 * The descriptor of an open file keeps the block it used last, and the tests
 * here count the reads of the device and say that a sequential read is a
 * step per block, while the content stays the content through a seek back,
 * a read across the edge of a block, a truncate, an append and a write into
 * the middle.
 *
 * The device of the volume is the RAM one, which counts the blocks it reads
 * and writes: that counter is the witness, not a clock of the host.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"

/* The file of the tests: forty blocks of data and a pattern in every byte. */
#define CHAIN_BLOCKS 40u
#define CHAIN_DATA (MFS_BLOCK_SIZE - 4)
#define CHAIN_BYTES (CHAIN_BLOCKS * CHAIN_DATA)

/* The volume: 2048 blocks of 1 KB, the same shape the board is formatted in. */
#define CHAIN_VOLUME_BLOCKS 2048u
#define CHAIN_RECORDS 256u

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;
static k_u8_t g_written[CHAIN_BYTES];
/* The reader has a little room past the file: the tail of the append and the
 * reads that cross the last block of the pattern end there. */
static k_u8_t g_read[CHAIN_BYTES + 1024u];

static int setup(void)
{
    int rc = mfs_ram_init(&g_volume, CHAIN_VOLUME_BLOCKS);

    if (rc != 0) {
        return rc;
    }

    mfs_ram_device(&g_volume, &g_device);

    return mfs_format(&g_device, CHAIN_RECORDS);
}

static void teardown(void)
{
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/*
 * The byte of a pattern. The block and the place in it are both in the
 * byte, so a read that slipped by one block, or by one place, is a byte the
 * test sees and a comment of "the content is the same" cannot hide.
 */
static k_u8_t pattern_at(k_u32_t offset)
{
    return (k_u8_t)((offset / CHAIN_DATA) * 37u + (offset % CHAIN_DATA) * 11u + 3u);
}

static void fill_pattern(k_u8_t *out, k_u32_t size)
{
    k_u32_t i;

    for (i = 0; i < size; i++) {
        out[i] = pattern_at(i);
    }
}

/* Write the whole buffer through the descriptor, whatever the block edges. */
static int write_all(struct mfs_file *file, const k_u8_t *data, k_u32_t size)
{
    k_u32_t done = 0;

    while (done < size) {
        k_size_t chunk = size - done;
        int rc;

        if (chunk > CHAIN_DATA) {
            chunk = CHAIN_DATA;
        }

        rc = mfs_write(file, data + done, chunk);
        if (rc <= 0) {
            return rc < 0 ? rc : (int)done;
        }

        done += (k_u32_t)rc;
    }

    return (int)done;
}

/* The bytes of the pattern from `from` are the bytes that were read. */
static bool content_is(k_u32_t from, k_u32_t size)
{
    k_u32_t i;

    for (i = 0; i < size; i++) {
        if (g_read[i] != pattern_at(from + i)) {
            return false;
        }
    }

    return true;
}

/* The volume is sound: no chain is broken and no block is lost. */
static void expect_sound_volume(void)
{
    struct mfs_check_report report;

    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0);
}

/* The reads of the device that the call made. */
static k_u32_t reads_of(k_u32_t before)
{
    return g_volume.reads - before;
}

/*
 * The file of the test, written and closed. Every test opens it again with
 * the flags it needs; a failure of the preparation is a failure of the test.
 */
static void make_chain_file(void)
{
    struct mfs_file file;

    fill_pattern(g_written, sizeof(g_written));

    KT_CHECK_INT_EQ(mfs_create("/chain.bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDWR, &file), MFS_OK);
    KT_CHECK_INT_EQ(write_all(&file, g_written, sizeof(g_written)), (int)sizeof(g_written));
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
}

/* The `size` bytes of the pattern, read from the head of the descriptor. */
static void read_and_check(struct mfs_file *file, k_u32_t size)
{
    memset(g_read, 0, CHAIN_BYTES);
    KT_CHECK_INT_EQ(mfs_read(file, g_read, size), (int)size);
    KT_CHECK(content_is(0, size));
}

/*
 * The property: reading a file of K blocks is a walk of the chain that goes
 * forward, so it costs about K reads of the device and not K*K/2. The limit
 * is four reads per block: one for the header of the next block, one for the
 * data, and the room for the bookkeeping of the filesystem. A read that
 * walks from the head would need half of the square, which is 820 reads for
 * the forty blocks here - far above the limit and far above the noise.
 */
static void test_read_is_linear(void)
{
    struct mfs_file file;
    k_u32_t before;
    k_u32_t reads;
    k_u32_t i;

    ktest_begin("mfs: a sequential read costs one step of the chain per block");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    make_chain_file();

    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDONLY, &file), MFS_OK);

    before = g_volume.reads;
    read_and_check(&file, CHAIN_BYTES);
    reads = reads_of(before);

    KT_CHECK(reads >= CHAIN_BLOCKS);
    KT_CHECK(reads <= 4u * CHAIN_BLOCKS + 8u);

    /* A seek back to the head walks once and then goes forward again; the
     * second pass has the same cost as the first one. */
    file.offset = 0;
    before = g_volume.reads;
    read_and_check(&file, CHAIN_BYTES);
    reads = reads_of(before);

    KT_CHECK(reads <= 4u * CHAIN_BLOCKS + 8u);

    /*
     * The same file a block at a time, the way a program reads it: the
     * cursor has to live in the descriptor from one call to the next, or
     * every call of a program that reads 1 KB pays the whole walk again.
     */
    file.offset = 0;
    memset(g_read, 0, CHAIN_BYTES);
    before = g_volume.reads;
    for (i = 0; i < CHAIN_BLOCKS; i++) {
        KT_CHECK_INT_EQ(mfs_read(&file, g_read + i * CHAIN_DATA, CHAIN_DATA), (int)CHAIN_DATA);
    }
    reads = reads_of(before);

    KT_CHECK(content_is(0, CHAIN_BYTES));
    KT_CHECK(reads <= 4u * CHAIN_BLOCKS + 8u);

    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    teardown();
}

/*
 * The property: a seek moves the cursor of the chain, and the two
 * directions reach the same bytes. Forwards is a step from where the
 * descriptor is; backwards is a walk from the head, still O(blocks).
 */
static void test_seek_and_block_edge(void)
{
    struct mfs_file file;
    k_u32_t before;
    k_u32_t reads;
    k_u32_t start;

    ktest_begin("mfs: a seek back and a read across the edge of a block keep the bytes");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    make_chain_file();

    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDONLY, &file), MFS_OK);

    /* The whole file first: the cursor ends at the last block. */
    read_and_check(&file, CHAIN_BYTES);

    /* Back into the middle of a block, for a read that crosses the edge of
     * two blocks: 500 bytes into block 3 and 2000 bytes out of it. */
    start = 3u * CHAIN_DATA + 500u;
    file.offset = start;
    memset(g_read, 0, 200u);
    before = g_volume.reads;
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, 2000), 2000);
    reads = reads_of(before);
    KT_CHECK(content_is(start, 2000));
    KT_CHECK(reads >= 3u); /* the walk from the head to block 3 */
    KT_CHECK(reads <= 2u * CHAIN_BLOCKS);

    /* Forward from the cursor of the read above, deeper into the file: the
     * blocks between the two places are walked, one step each, and nothing
     * starts again at the head. */
    start = CHAIN_BYTES - 2u * CHAIN_DATA;
    file.offset = start;
    before = g_volume.reads;
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, 2u * CHAIN_DATA), (int)(2u * CHAIN_DATA));
    reads = reads_of(before);
    KT_CHECK(content_is(start, 2u * CHAIN_DATA));
    KT_CHECK(reads <= 2u * CHAIN_BLOCKS);

    /* Back to the head again, and the same bytes one more time. */
    file.offset = 0;
    read_and_check(&file, CHAIN_BYTES);

    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    teardown();
}

/*
 * The property: an append after a read is added to the end of the chain the
 * cursor knows, and the blocks before it keep their bytes. The write of the
 * append changes the header of the last block, so a cursor that pointed at
 * it has to follow the new block, not the old end.
 */
static void test_append_after_read(void)
{
    struct mfs_file file;
    k_u8_t tail[600];
    k_u32_t start;

    ktest_begin("mfs: an append after a read keeps the chain and the bytes");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    make_chain_file();

    memset(tail, 'A', sizeof(tail));

    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDWR | MFS_O_APPEND, &file), MFS_OK);
    KT_CHECK_INT_EQ((int)file.offset, (int)CHAIN_BYTES); /* append starts at the end */

    /* The cursor walks the whole chain, and stops on the last block. */
    file.offset = 0;
    read_and_check(&file, CHAIN_BYTES);

    /* The append goes to the end, whatever the position of the descriptor. */
    KT_CHECK_INT_EQ(mfs_write(&file, tail, sizeof(tail)), (int)sizeof(tail));
    KT_CHECK_INT_EQ((int)file.size, (int)(CHAIN_BYTES + sizeof(tail)));

    /* The end of the read is the end of the file: nothing to read. */
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, 1), 0);

    /* A seek back into the last block of the old file, for a read that
     * crosses from the old last block into the appended bytes. */
    start = CHAIN_BYTES - 100u;
    file.offset = start;
    memset(g_read, 0, 700);
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, 700), 700);

    {
        int good = 1;
        k_u32_t i;

        for (i = 0; i < 100; i++) {
            if (g_read[i] != pattern_at(start + i)) {
                good = 0;
            }
        }
        for (i = 0; i < sizeof(tail); i++) {
            if (g_read[100u + i] != 'A') {
                good = 0;
            }
        }
        KT_CHECK(good);
    }

    /* The whole file from the head: the old pattern, then the tail. */
    file.offset = 0;
    memset(g_read, 0, CHAIN_BYTES + sizeof(tail));
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, CHAIN_BYTES + sizeof(tail)),
                    (int)(CHAIN_BYTES + sizeof(tail)));
    KT_CHECK(content_is(0, CHAIN_BYTES));
    KT_CHECK_INT_EQ(memcmp(g_read + CHAIN_BYTES, tail, sizeof(tail)), 0);

    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* A descriptor of the new size reads the same bytes, and the volume is
     * still sound: the append did not leave a block outside the chain. */
    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ((int)file.size, (int)(CHAIN_BYTES + sizeof(tail)));
    file.offset = CHAIN_BYTES;
    memset(g_read, 0, sizeof(tail));
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, sizeof(tail)), (int)sizeof(tail));
    KT_CHECK_INT_EQ(memcmp(g_read, tail, sizeof(tail)), 0);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    expect_sound_volume();

    teardown();
}

/*
 * The property: a truncate is a new chain, and the cursor of a descriptor
 * that the open made cannot name a block of the chain that was freed. The
 * file of the test is cut to nothing by the open, written again and read
 * from the head of the new chain.
 */
static void test_truncate_drops_the_cursor(void)
{
    struct mfs_file file;
    k_u32_t before;
    k_u32_t reads;

    ktest_begin("mfs: a truncate gives the file a new chain and the read follows it");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    make_chain_file();

    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDONLY, &file), MFS_OK);
    read_and_check(&file, CHAIN_BYTES);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* The open with "trunc" frees the old chain and starts the file again. */
    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDWR | MFS_O_TRUNC, &file), MFS_OK);
    KT_CHECK_INT_EQ((int)file.size, 0);
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, CHAIN_BYTES), 0);

    /* Three blocks of the same pattern, written into the new chain. */
    KT_CHECK_INT_EQ(write_all(&file, g_written, 3u * CHAIN_DATA), (int)(3u * CHAIN_DATA));
    KT_CHECK_INT_EQ((int)file.size, (int)(3u * CHAIN_DATA));

    file.offset = 0;
    memset(g_read, 0, CHAIN_BYTES);
    before = g_volume.reads;
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, 3u * CHAIN_DATA), (int)(3u * CHAIN_DATA));
    reads = reads_of(before);
    KT_CHECK(content_is(0, 3u * CHAIN_DATA));
    KT_CHECK(reads <= 4u * CHAIN_BLOCKS + 8u);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* A later reader sees the same three blocks, and the volume is sound. */
    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ((int)file.size, (int)(3u * CHAIN_DATA));
    read_and_check(&file, 3u * CHAIN_DATA);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    expect_sound_volume();

    teardown();
}

/*
 * The property: a write into the middle after a read changes the bytes of
 * the block and not the chain, so the cursor stays where it was and the
 * next read, forward or from the head, gives the old bytes around the new
 * ones.
 */
static void test_write_middle_after_read(void)
{
    struct mfs_file file;
    k_u8_t middle[100];
    k_u32_t start = 6u * CHAIN_DATA + 200u;
    k_u32_t i;

    ktest_begin("mfs: a write into the middle after a read keeps the rest of the file");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    make_chain_file();

    memset(middle, 0xEE, sizeof(middle));

    KT_CHECK_INT_EQ(mfs_open("/chain.bin", MFS_O_RDWR, &file), MFS_OK);
    read_and_check(&file, CHAIN_BYTES);

    /* The cursor is at the tail; the write goes back into block 6 and does
     * not touch the chain. */
    file.offset = start;
    KT_CHECK_INT_EQ(mfs_write(&file, middle, sizeof(middle)), (int)sizeof(middle));
    KT_CHECK_INT_EQ((int)file.size, (int)CHAIN_BYTES);

    file.offset = 0;
    memset(g_read, 0, CHAIN_BYTES);
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, CHAIN_BYTES), (int)CHAIN_BYTES);
    KT_CHECK_INT_EQ(mfs_read(&file, g_read, 1), 0);

    {
        int good = 1;

        for (i = 0; i < CHAIN_BYTES; i++) {
            k_u8_t expected = (i >= start && i < start + sizeof(middle)) ? 0xEE : pattern_at(i);

            if (g_read[i] != expected) {
                good = 0;
            }
        }
        KT_CHECK(good);
    }

    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    expect_sound_volume();

    teardown();
}

/*
 * The property: a file written a block at a time walks the chain about once
 * and not once per call. Two walks were the cost of every call: the count of
 * the blocks of the chain and the search for its last block. The descriptor
 * keeps the count after the first write, and the block the growth added is
 * the one the cursor names, so the forty blocks of the test cost one step
 * each. A write that counted and walked from the head every time would need
 * about half of the square, which is 820 reads for the forty blocks here -
 * far above the limit and far above the noise.
 */
static void test_write_is_linear(void)
{
    struct mfs_file file;
    k_u32_t before;
    k_u32_t reads;
    k_u32_t i;

    ktest_begin("mfs: a file written a block at a time walks the chain once");

    KT_CHECK_INT_EQ(setup(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    fill_pattern(g_written, sizeof(g_written));

    KT_CHECK_INT_EQ(mfs_create("/grown.bin"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/grown.bin", MFS_O_RDWR, &file), MFS_OK);

    before = g_volume.reads;
    for (i = 0; i < CHAIN_BLOCKS; i++) {
        KT_CHECK_INT_EQ(mfs_write(&file, g_written + i * CHAIN_DATA, CHAIN_DATA), (int)CHAIN_DATA);
    }
    reads = reads_of(before);

    KT_CHECK(reads >= CHAIN_BLOCKS);
    KT_CHECK(reads <= 4u * CHAIN_BLOCKS + 8u);

    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    /* The file is what was written, block for block. */
    KT_CHECK_INT_EQ(mfs_open("/grown.bin", MFS_O_RDONLY, &file), MFS_OK);
    read_and_check(&file, CHAIN_BYTES);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    expect_sound_volume();

    teardown();
}

void test_mfs_chain(void)
{
    test_read_is_linear();
    test_seek_and_block_edge();
    test_append_after_read();
    test_truncate_drops_the_cursor();
    test_write_middle_after_read();
    test_write_is_linear();
}
