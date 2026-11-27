/*
 * Micro OS — the journal of MicroFS, version 4 (ADR 0047).
 *
 * The test is a power cut with a memory: the volume is written by the real
 * filesystem, and once the cut point is reached the device accepts the
 * writes of the driver and loses them, the way a cache of a disk that lost
 * its power does. The filesystem is not told: its calls answer the way they
 * always do, and the machine is "reset" by mounting the same blocks again.
 *
 * Every cut point of a write of a file is tried, from the first block of the
 * record to the last write of the close. The promise of the ADR is the same
 * at every point: the mount succeeds, the check finds the volume
 * consistent, the file that was whole before the draft is whole, and the
 * draft is either whole or not there at all.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdlib.h>
#include <string.h>

#include "kernel/blockdev.h"
#include "kernel/types.h"
#include "mfs/mfs.h"

/* A volume of 512 KiB: small enough to try every write of a file, large
 * enough for the table, the journal and 48 KiB of data. */
#define CUT_BLOCKS 512u
#define KEEP_BYTES 32u
#define DRAFT_BYTES (48u * 1024u)
#define CHUNK 1024u

static k_u8_t *g_volume;
static k_u8_t *g_base;
static k_u32_t g_writes;
static k_u32_t g_limit;
static bool g_dropped;

static int cut_read(void *ctx, k_u32_t sector, void *buf, k_size_t count)
{
    (void)ctx;

    if (sector >= CUT_BLOCKS || count > CUT_BLOCKS - sector) {
        return -1;
    }

    memcpy(buf, g_volume + (k_size_t)sector * MFS_BLOCK_SIZE, count * MFS_BLOCK_SIZE);

    return 0;
}

static int cut_write(void *ctx, k_u32_t sector, const void *buf, k_size_t count)
{
    (void)ctx;

    if (sector >= CUT_BLOCKS || count > CUT_BLOCKS - sector) {
        return -1;
    }

    g_writes++;

    if (g_writes > g_limit) {
        /* The power went: the caller is told the write happened, and the
         * bytes never reach the medium. */
        g_dropped = true;
        return 0;
    }

    memcpy(g_volume + (k_size_t)sector * MFS_BLOCK_SIZE, buf, count * MFS_BLOCK_SIZE);

    return 0;
}

static int cut_erase(void *ctx, k_u32_t sector, k_size_t count)
{
    (void)ctx;

    if (sector >= CUT_BLOCKS || count > CUT_BLOCKS - sector) {
        return -1;
    }

    if (g_writes++ > g_limit) {
        g_dropped = true;
        return 0;
    }

    memset(g_volume + (k_size_t)sector * MFS_BLOCK_SIZE, 0xFF, count * MFS_BLOCK_SIZE);

    return 0;
}

static int cut_sync(void *ctx)
{
    (void)ctx;

    return 0;
}

static const struct k_blockdev_ops g_cut_ops = {
    .read = cut_read,
    .write = cut_write,
    .erase = cut_erase,
    .sync = cut_sync,
};

static struct k_blockdev g_device;

static void pattern(k_u8_t *out, k_size_t offset, k_size_t size)
{
    k_size_t i;

    for (i = 0; i < size; i++) {
        out[i] = (k_u8_t)((offset + i) * 7u + 13u);
    }
}

static bool volume_alloc(void)
{
    g_volume = (k_u8_t *)malloc((size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);
    g_base = (k_u8_t *)malloc((size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);

    return g_volume != NULL && g_base != NULL;
}

static void volume_free(void)
{
    free(g_volume);
    free(g_base);
    g_volume = NULL;
    g_base = NULL;
}

/* A volume the filesystem writes, with the one file that has to survive. */
static int volume_make(void)
{
    struct mfs_file file;
    k_u8_t text[KEEP_BYTES];
    int rc;

    g_writes = 0;
    g_limit = 0xFFFFFFFFu;
    g_dropped = false;

    g_device.name = "cut";
    g_device.sector_size = MFS_BLOCK_SIZE;
    g_device.sectors = CUT_BLOCKS;
    g_device.ops = &g_cut_ops;
    g_device.ctx = NULL;

    memset(g_volume, 0, (size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);

    rc = mfs_format_ex(&g_device, 256, MFS_FORMAT_JOURNAL);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = mfs_mount(&g_device);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = mfs_create("/keep.txt");
    if (rc != MFS_OK) {
        return rc;
    }

    rc = mfs_open("/keep.txt", MFS_O_WRONLY, &file);
    if (rc != MFS_OK) {
        return rc;
    }

    pattern(text, 0, sizeof(text));

    if (mfs_write(&file, text, sizeof(text)) != (int)sizeof(text)) {
        return MFS_EIO;
    }

    rc = mfs_close(&file);
    if (rc != MFS_OK) {
        return rc;
    }

    rc = mfs_sync();
    if (rc != MFS_OK) {
        return rc;
    }

    mfs_unmount();

    memcpy(g_base, g_volume, (size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);
    g_writes = 0;

    return MFS_OK;
}

/* The write of the draft: create, write it in chunks, close it. */
static void session_write(void)
{
    struct mfs_file file;
    k_u8_t chunk[CHUNK];
    k_size_t offset;
    int rc;

    rc = mfs_open("/draft.bin", MFS_O_WRONLY | MFS_O_CREAT | MFS_O_TRUNC, &file);
    if (rc != MFS_OK) {
        return;
    }

    for (offset = 0; offset < DRAFT_BYTES; offset += CHUNK) {
        pattern(chunk, offset, sizeof(chunk));

        if (mfs_write(&file, chunk, sizeof(chunk)) != (int)sizeof(chunk)) {
            return;
        }
    }

    (void)mfs_close(&file);
}

/* The file that was whole before the draft: it has to be whole after. */
static bool keep_whole(void)
{
    struct mfs_file file;
    k_u8_t text[KEEP_BYTES];
    k_u8_t want[KEEP_BYTES];

    if (mfs_open("/keep.txt", MFS_O_RDONLY, &file) != MFS_OK) {
        return false;
    }

    pattern(want, 0, sizeof(want));

    if (mfs_read(&file, text, sizeof(text)) != (int)sizeof(text)) {
        mfs_close(&file);
        return false;
    }

    mfs_close(&file);

    return memcmp(text, want, sizeof(text)) == 0;
}

static bool draft_absent(void)
{
    struct mfs_inode inode;

    return mfs_stat("/draft.bin", &inode, NULL) == MFS_ENOENT;
}

static bool draft_whole(void)
{
    struct mfs_file file;
    struct mfs_inode inode;
    k_u8_t chunk[CHUNK];
    k_u8_t want[CHUNK];
    k_size_t offset;
    bool whole = true;

    if (mfs_stat("/draft.bin", &inode, NULL) != MFS_OK) {
        return false;
    }

    if (inode.size != DRAFT_BYTES) {
        return false;
    }

    if (mfs_open("/draft.bin", MFS_O_RDONLY, &file) != MFS_OK) {
        return false;
    }

    for (offset = 0; offset < DRAFT_BYTES && whole; offset += CHUNK) {
        if (mfs_read(&file, chunk, sizeof(chunk)) != (int)sizeof(chunk)) {
            whole = false;
            break;
        }

        pattern(want, offset, sizeof(want));

        if (memcmp(chunk, want, sizeof(want)) != 0) {
            whole = false;
        }
    }

    mfs_close(&file);

    return whole;
}

static void test_journal_format(void)
{
    struct mfs_superblock sb;
    struct mfs_journal_header header;
    k_u8_t block[MFS_BLOCK_SIZE];
    struct mfs_check_report report;

    ktest_begin("mfs journal: a volume of version 4 has a journal in META");

    KT_CHECK(volume_alloc());
    if (g_volume == NULL || g_base == NULL) {
        volume_free();
        return;
    }

    KT_CHECK_INT_EQ(volume_make(), MFS_OK);

    KT_CHECK_INT_EQ(cut_read(NULL, CUT_BLOCKS - 1u, block, 1), 0);
    memcpy(&sb, block, sizeof(sb));

    KT_CHECK_INT_EQ(sb.version, MFS_VERSION_JOURNAL);
    KT_CHECK((sb.features & MFS_FEATURE_JOURNAL) != 0u);
    KT_CHECK_INT_EQ(sb.journal_blocks, 1u + sb.journal_slots * 2u);
    KT_CHECK_INT_EQ(sb.bitmap_blocks + sb.inode_blocks + sb.journal_blocks, sb.meta_blocks);
    KT_CHECK_INT_EQ(sb.journal_start + sb.journal_blocks, sb.info_start - sb.bitmap_blocks);
    KT_CHECK(sb.journal_slots >= MFS_JOURNAL_EXTRA_SLOTS);

    /* The header of the journal is the empty one: nothing to replay. */
    KT_CHECK_INT_EQ(cut_read(NULL, sb.journal_start, block, 1), 0);
    memcpy(&header, block, sizeof(header));
    KT_CHECK_INT_EQ(header.magic, MFS_JOURNAL_MAGIC);
    KT_CHECK_INT_EQ(header.version, MFS_JOURNAL_VERSION);
    KT_CHECK_INT_EQ(header.state, MFS_JOURNAL_STATE_EMPTY);
    KT_CHECK_INT_EQ(header.slots, 0u);

    /* A volume of version 3 is still made when the flag is not asked. */
    KT_CHECK_INT_EQ(mfs_format(&g_device, 256), MFS_OK);
    KT_CHECK_INT_EQ(cut_read(NULL, CUT_BLOCKS - 1u, block, 1), 0);
    memcpy(&sb, block, sizeof(sb));
    KT_CHECK_INT_EQ(sb.version, MFS_VERSION);
    KT_CHECK_INT_EQ(sb.journal_blocks, 0u);
    KT_CHECK_INT_EQ(sb.journal_start, 0u);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    memset(&report, 0, sizeof(report));
    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK_INT_EQ(report.errors, 0u);
    KT_CHECK_INT_EQ(report.journal_replayed, 0u);
    mfs_unmount();

    volume_free();
}

/*
 * The cut of a write of a file, at every one of its writes. The device of
 * the test loses every write after the point, and the filesystem is mounted
 * again over the blocks it left: the mount is the second boot of a machine
 * whose battery died.
 */
static void test_journal_cut(void)
{
    k_u32_t session_writes;
    k_u32_t cut;
    k_u32_t absent = 0;
    k_u32_t whole = 0;
    k_u32_t replayed = 0;

    ktest_begin("mfs journal: a cut of a write leaves the file whole or absent");

    KT_CHECK(volume_alloc());
    if (g_volume == NULL || g_base == NULL) {
        volume_free();
        return;
    }

    KT_CHECK_INT_EQ(volume_make(), MFS_OK);

    /* One session without a cut says how many writes the loop has to try. */
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    session_write();
    session_writes = g_writes;
    KT_CHECK(session_writes > 10u);

    for (cut = 0; cut <= session_writes; cut++) {
        struct mfs_check_report report;
        bool ok = true;

        memcpy(g_volume, g_base, (size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);
        g_writes = 0;
        g_limit = cut;
        g_dropped = false;

        /* The first boot: the program writes and the power goes at `cut`. */
        if (mfs_mount(&g_device) != MFS_OK) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the first mount failed", (unsigned)cut);
            continue;
        }

        session_write();

        /*
         * The power is back: the writes of the second boot are the writes of
         * a machine that is on, and the device is not dropping anything any
         * more. The mount is the reset the battery gave the volume.
         */
        g_limit = 0xFFFFFFFFu;

        if (mfs_mount(&g_device) != MFS_OK) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the volume did not mount again after the cut",
                       (unsigned)cut);
            continue;
        }

        memset(&report, 0, sizeof(report));
        if (mfs_check(&report) != MFS_OK || report.errors != 0u) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the check found %u problem(s): %s",
                       (unsigned)cut, (unsigned)report.errors,
                       report.first[0] != '\0' ? report.first : "(none)");
            ok = false;
        }

        if (report.journal_replayed != 0u) {
            replayed++;
        }

        if (!keep_whole()) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the file written before the cut is damaged",
                       (unsigned)cut);
            ok = false;
        }

        if (draft_absent()) {
            absent++;
        } else if (draft_whole()) {
            whole++;
        } else {
            struct mfs_inode ino;
            (void)mfs_stat("/draft.bin", &ino, NULL);
            ktest_fail(__FILE__, __LINE__,
                       "cut %u: the draft is neither whole nor absent (size %u flags 0x%x)",
                       (unsigned)cut, (unsigned)ino.size, (unsigned)ino.flags);
            ok = false;
        }

        if (!ok) {
            break;
        }
    }

    mfs_unmount();

    /* Both ends of the sweep are real: some cuts keep the close, some lose
     * it, and every one of them is one of the two answers of the promise. */
    KT_CHECK(absent > 0u);
    KT_CHECK(whole > 0u);
    KT_CHECK(replayed > 0u);
    KT_CHECK_INT_EQ(absent + whole, session_writes + 1u);

    volume_free();
}

/*
 * A metadata operation is atomic: a rename that a cut interrupts leaves the
 * object under its old name or under the new one, never under both and never
 * under neither, and the check of the volume finds nothing.
 */
static void test_journal_rename(void)
{
    struct mfs_file file;
    k_u32_t cut;
    k_u32_t session_writes;
    k_u32_t old_name = 0;
    k_u32_t new_name = 0;

    ktest_begin("mfs journal: a cut of a rename leaves one name and one whole file");

    KT_CHECK(volume_alloc());
    if (g_volume == NULL || g_base == NULL) {
        volume_free();
        return;
    }

    KT_CHECK_INT_EQ(volume_make(), MFS_OK);

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/a.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK((mfs_write(&file, "hello", 5)) == 5);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_sync(), MFS_OK);
    mfs_unmount();

    memcpy(g_base, g_volume, (size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);

    /* One rename without a cut says how many writes it has. */
    g_writes = 0;
    g_limit = 0xFFFFFFFFu;
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rename("/a.txt", "/b.txt"), MFS_OK);
    session_writes = g_writes;
    mfs_unmount();

    for (cut = 0; cut <= session_writes; cut++) {
        struct mfs_check_report report;
        struct mfs_inode inode;
        bool has_a;
        bool has_b;

        memcpy(g_volume, g_base, (size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);
        g_writes = 0;
        g_limit = cut;

        KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
        (void)mfs_rename("/a.txt", "/b.txt");

        g_limit = 0xFFFFFFFFu;

        if (mfs_mount(&g_device) != MFS_OK) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the volume did not mount again", (unsigned)cut);
            break;
        }

        memset(&report, 0, sizeof(report));
        if (mfs_check(&report) != MFS_OK || report.errors != 0u) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the check found %u problem(s): %s",
                       (unsigned)cut, (unsigned)report.errors,
                       report.first[0] != '\0' ? report.first : "(none)");
            break;
        }

        has_a = mfs_stat("/a.txt", &inode, NULL) == MFS_OK;
        has_b = mfs_stat("/b.txt", &inode, NULL) == MFS_OK;

        if (has_a == has_b) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the names are %d and %d", (unsigned)cut,
                       (int)has_a, (int)has_b);
            break;
        }

        if (has_a) {
            old_name++;
        } else {
            new_name++;
        }
    }

    mfs_unmount();

    KT_CHECK(old_name > 0u);
    KT_CHECK(new_name > 0u);

    volume_free();
}

/*
 * Fill the stack of this thread with a pattern and return. The call after it
 * - the rename - walks frames of the same depth, and a byte that a structure
 * of the filesystem forgot to write shows up as the pattern and not as a
 * zero of a lucky frame.
 */
static void stack_noise(void)
{
    volatile k_u8_t noise[8192];
    k_size_t i;

    for (i = 0; i < sizeof(noise); i++) {
        noise[i] = (k_u8_t)0x5Au;
    }
}

/*
 * The two properties a block of the metadata cannot lose without a byte of
 * the medium being undefined:
 *
 *   - the header of the volume is the same bytes when the same volume is
 *     formatted twice: every field of it is written, and its checksum is
 *     taken over bytes the format defines (no padding of a structure);
 *   - the reserved byte of a directory entry - the byte after the type - is
 *     zero, whatever object wrote the entry. A rename takes the entry of the
 *     lookup and writes it again, and the lookup does not fill the reserved
 *     byte; the writer itself has to zero it, or the journal checksums a byte
 *     of a stack frame (the defect the valgrind pass of 1.9 found).
 */
static void test_journal_bytes(void)
{
    struct mfs_file file;
    k_u8_t first[MFS_BLOCK_SIZE];
    k_u32_t block;
    k_u32_t offset;
    k_u32_t entries = 0;

    ktest_begin("mfs journal: the metadata carries no byte that nobody wrote");

    KT_CHECK(volume_alloc());
    if (g_volume == NULL || g_base == NULL) {
        volume_free();
        return;
    }

    KT_CHECK_INT_EQ(volume_make(), MFS_OK);

    /* The header of a volume is the bytes of its fields: format it again and
     * the block is the same. */
    KT_CHECK_INT_EQ(cut_read(NULL, CUT_BLOCKS - 1u, first, 1), 0);
    KT_CHECK_INT_EQ(mfs_format_ex(&g_device, 256, MFS_FORMAT_JOURNAL), MFS_OK);
    {
        k_u8_t again[MFS_BLOCK_SIZE];

        KT_CHECK_INT_EQ(cut_read(NULL, CUT_BLOCKS - 1u, again, 1), 0);
        KT_CHECK(memcmp(first, again, MFS_BLOCK_SIZE) == 0);
    }

    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/a.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/a.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK(mfs_write(&file, "hello", 5) == 5);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    stack_noise();
    KT_CHECK_INT_EQ(mfs_rename("/a.txt", "/b.txt"), MFS_OK);
    mfs_unmount();

    /* The entry of b.txt on the medium: the eight bytes of its header and the
     * name. Every one of them is a byte the format names, and the reserved
     * one is zero. */
    for (block = 0; block < CUT_BLOCKS; block++) {
        const k_u8_t *data = g_volume + (k_size_t)block * MFS_BLOCK_SIZE;

        for (offset = 0; offset + 8u + 5u <= MFS_BLOCK_SIZE; offset++) {
            k_u16_t name_len;

            memcpy(&name_len, data + offset + 4, sizeof(name_len));

            if (name_len != 5u || memcmp(data + offset + 8u, "b.txt", 5u) != 0) {
                continue;
            }

            entries++;
            KT_CHECK_INT_EQ(data[offset + 4], 5u);
            KT_CHECK_INT_EQ(data[offset + 5], 0u);
            KT_CHECK_INT_EQ(data[offset + 7], 0u);
            KT_CHECK_INT_EQ(data[offset + 6] & MFS_INODE_DIR, 0u);
        }
    }

    KT_CHECK(entries > 0u);

    volume_free();
}

/*
 * The most ordinary sequence of the filesystem: write a file, close it, open
 * it again and read it back - a hundred times, on eight names that are made
 * and cut again.
 *
 * The deadlock of the x86-64 machine that this lane found is a property of
 * the port's mutex and not of this one (a task waiting for a lock held by the
 * context the core booted on, which is not in a run queue); the host port has
 * no such context and cannot show it. What the loop does show is the state of
 * the journal after the sequence a person runs all day: a transaction that
 * leaves the record, the map or the directory behind fails here first.
 *
 * The second half is the file of a write that was not closed: a reader of the
 * same object sees the record the close has not published - size zero - and
 * the whole content arrives with the close. Neither a hang nor a wrong size
 * is allowed.
 */
static void test_journal_write_read(void)
{
    struct mfs_file file;
    struct mfs_inode inode;
    char path[32];
    char text[64];
    char back[64];
    int i;

    ktest_begin("mfs journal: a file is written, closed and read again");

    KT_CHECK(volume_alloc());
    if (g_volume == NULL || g_base == NULL) {
        volume_free();
        return;
    }

    KT_CHECK_INT_EQ(volume_make(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);

    for (i = 0; i < 100; i++) {
        int rc;

        snprintf(path, sizeof(path), "/f%d.txt", i % 8);
        memset(text, 'a' + (i % 26), sizeof(text));
        text[sizeof(text) - 1] = '\0';

        rc = mfs_create(path);
        KT_CHECK(rc == MFS_OK || rc == MFS_EEXIST);
        KT_CHECK_INT_EQ(mfs_open(path, MFS_O_WRONLY | MFS_O_TRUNC, &file), MFS_OK);
        KT_CHECK(mfs_write(&file, text, sizeof(text)) == (int)sizeof(text));
        KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

        KT_CHECK_INT_EQ(mfs_open(path, MFS_O_RDONLY, &file), MFS_OK);
        memset(back, 0, sizeof(back));
        KT_CHECK(mfs_read(&file, back, sizeof(back)) == (int)sizeof(back));
        KT_CHECK(memcmp(back, text, sizeof(text)) == 0);
        KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    }

    /* A write that was not closed: the record says zero until the close. */
    KT_CHECK_INT_EQ(mfs_create("/open.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/open.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK(mfs_write(&file, "unfinished", 10) == 10);
    KT_CHECK_INT_EQ(mfs_stat("/open.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 0u);

    {
        struct mfs_file reader;

        KT_CHECK_INT_EQ(mfs_open("/open.txt", MFS_O_RDONLY, &reader), MFS_OK);
        KT_CHECK_INT_EQ(mfs_read(&reader, back, sizeof(back)), 0);
        KT_CHECK_INT_EQ(mfs_close(&reader), MFS_OK);
    }

    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_stat("/open.txt", &inode, NULL), MFS_OK);
    KT_CHECK_INT_EQ(inode.size, 10u);

    KT_CHECK_INT_EQ(mfs_open("/open.txt", MFS_O_RDONLY, &file), MFS_OK);
    memset(back, 0, sizeof(back));
    KT_CHECK(mfs_read(&file, back, 10) == 10);
    KT_CHECK(memcmp(back, "unfinished", 10) == 0);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);

    mfs_unmount();
    volume_free();
}

/*
 * Replacing an existing name adds one more metadata change to the rename: the
 * old target's entry, inode and block all disappear. Every cut point must leave
 * either the old pair or the new name, and the check must find a clean volume.
 */
static void test_journal_rename_replace(void)
{
    struct mfs_file file;
    struct mfs_inode inode;
    struct mfs_check_report report;
    k_u8_t expected[KEEP_BYTES];
    char text[KEEP_BYTES];
    k_u32_t cut;
    k_u32_t session_writes;
    k_u32_t old_name = 0u;
    k_u32_t new_name = 0u;

    ktest_begin("mfs journal: a cut of a replacing rename leaves a clean volume");

    KT_CHECK(volume_alloc());
    if (g_volume == NULL || g_base == NULL) {
        volume_free();
        return;
    }

    KT_CHECK_INT_EQ(volume_make(), MFS_OK);
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_create("/target.txt"), MFS_OK);
    KT_CHECK_INT_EQ(mfs_open("/target.txt", MFS_O_WRONLY, &file), MFS_OK);
    KT_CHECK_INT_EQ(mfs_write(&file, "old target", 10), 10);
    KT_CHECK_INT_EQ(mfs_close(&file), MFS_OK);
    mfs_unmount();

    memcpy(g_base, g_volume, (size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);
    pattern(expected, 0, sizeof(expected));

    g_writes = 0u;
    g_limit = 0xFFFFFFFFu;
    KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
    KT_CHECK_INT_EQ(mfs_rename("/keep.txt", "/target.txt"), MFS_OK);
    session_writes = g_writes;
    mfs_unmount();

    for (cut = 0u; cut <= session_writes; cut++) {
        bool has_old_name;
        k_size_t used;

        memcpy(g_volume, g_base, (size_t)CUT_BLOCKS * MFS_BLOCK_SIZE);
        g_writes = 0u;
        g_limit = cut;
        g_dropped = false;

        KT_CHECK_INT_EQ(mfs_mount(&g_device), MFS_OK);
        (void)mfs_rename("/keep.txt", "/target.txt");
        g_limit = 0xFFFFFFFFu;

        if (mfs_mount(&g_device) != MFS_OK) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the volume did not mount again", (unsigned)cut);
            break;
        }

        memset(&report, 0, sizeof(report));
        if (mfs_check(&report) != MFS_OK || report.errors != 0u) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the check found %u problem(s): %s",
                       (unsigned)cut, (unsigned)report.errors,
                       report.first[0] != '\0' ? report.first : "(none)");
            break;
        }

        has_old_name = mfs_stat("/keep.txt", &inode, NULL) == MFS_OK;
        if (mfs_stat("/target.txt", &inode, NULL) != MFS_OK ||
            mfs_open("/target.txt", MFS_O_RDONLY, &file) != MFS_OK) {
            ktest_fail(__FILE__, __LINE__, "cut %u: the target name is gone", (unsigned)cut);
            break;
        }

        memset(text, 0, sizeof(text));
        used = (k_size_t)mfs_read(&file, text, sizeof(text));
        (void)mfs_close(&file);

        if (has_old_name) {
            if (used != 10u || memcmp(text, "old target", 10u) != 0) {
                ktest_fail(__FILE__, __LINE__, "cut %u: the old target changed", (unsigned)cut);
                break;
            }
            old_name++;
        } else {
            if (used != sizeof(expected) || memcmp(text, expected, sizeof(expected)) != 0) {
                ktest_fail(__FILE__, __LINE__, "cut %u: the new target is not whole",
                           (unsigned)cut);
                break;
            }
            new_name++;
        }
    }

    mfs_unmount();
    KT_CHECK(old_name > 0u);
    KT_CHECK(new_name > 0u);
    volume_free();
}

void test_mfs_journal(void)
{
    test_journal_format();
    test_journal_bytes();
    test_journal_write_read();
    test_journal_cut();
    test_journal_rename();
    test_journal_rename_replace();
}
