#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/clock.h"
#include "kernel/errno.h"
#include "kernel/sysinfo.h"
#include "kernel/vfs.h"
#include "ramfs/ramfs_vfs.h"

#define RAMFS_POINT "/"
#define RAMFS_TEST_FILE "/big.bin"

static k_u8_t g_file_data[VFS_FILE_DATA_SIZE];

static void setup(void)
{
    /* A machine without a battery has no time until it is told one: the case
     * gives it a moment of its own, so the stamp of a node does not depend on
     * the order of the tests or on the build. */
    (void)k_clock_set_unix(1700000000u);

    (void)vfs_umount(RAMFS_POINT);
    (void)ramfs_vfs_reset();
    KT_CHECK_INT_EQ(ramfs_vfs_register(RAMFS_POINT), 0);
}

static void teardown(void)
{
    (void)vfs_umount(RAMFS_POINT);
    (void)ramfs_vfs_reset();
}

static void test_write_read_8m(void)
{
    struct vfs_file file;
    k_u8_t buf[1024];
    k_size_t i;
    k_size_t written = 0;
    k_u32_t sum_write = 0;
    k_u32_t sum_read = 0;
    const k_size_t total = 8u * 1024u * 1024u;
    int rc;

    ktest_begin("ramfs: write 8 MiB and read back byte by byte (checksum)");

    setup();

    rc = vfs_open(RAMFS_TEST_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        teardown();
        return;
    }

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = (k_u8_t)(i & 0xffu);
    }

    while (written < total) {
        k_size_t chunk = sizeof(buf);
        k_size_t j;
        int n;

        if (chunk > total - written) {
            chunk = total - written;
        }

        /* Vary the pattern by offset so the checksum catches wrong reads. */
        buf[0] = (k_u8_t)(written & 0xffu);
        buf[1] = (k_u8_t)((written >> 8) & 0xffu);

        n = vfs_write(&file, buf, chunk);
        KT_CHECK(n > 0);
        if (n <= 0) {
            break;
        }

        for (j = 0; j < (k_size_t)n; j++) {
            sum_write += buf[j];
        }
        written += (k_size_t)n;
    }

    KT_CHECK_INT_EQ((int)written, (int)total);
    KT_CHECK_INT_EQ(vfs_close(&file), 0);

    rc = vfs_open(RAMFS_TEST_FILE, VFS_O_RDONLY, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        k_size_t read_total = 0;

        while (read_total < total) {
            int n = vfs_read(&file, buf, sizeof(buf));
            k_size_t j;

            KT_CHECK(n > 0);
            if (n <= 0) {
                break;
            }

            for (j = 0; j < (k_size_t)n; j++) {
                sum_read += buf[j];
            }
            read_total += (k_size_t)n;
        }

        KT_CHECK_INT_EQ((int)read_total, (int)total);
        KT_CHECK_INT_EQ((int)sum_read, (int)sum_write);
        (void)vfs_close(&file);
    }

    KT_CHECK_INT_EQ(vfs_unlink(RAMFS_TEST_FILE), 0);

    teardown();
}

static void test_unlink_returns_heap(void)
{
    struct vfs_file file;
    k_u8_t buf[1024];
    k_size_t before;
    k_size_t after;
    k_size_t written = 0;
    const k_size_t total = 1u * 1024u * 1024u;
    int rc;

    ktest_begin("ramfs: unlink returns heap to previous count");

    setup();
    memset(buf, 0x5a, sizeof(buf));

    before = k_sysinfo_heap_free();

    rc = vfs_open(RAMFS_TEST_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        while (written < total) {
            int n = vfs_write(&file, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            written += (k_size_t)n;
        }
        (void)vfs_close(&file);
    }

    KT_CHECK_INT_EQ((int)written, (int)total);
    KT_CHECK_INT_EQ(vfs_unlink(RAMFS_TEST_FILE), 0);

    after = k_sysinfo_heap_free();

    /*
     * The file, its blocks and the directory entry are all gone; the heap
     * should be back where it was. Allow a small slack for allocator
     * bookkeeping that may not be byte-identical across a large churn.
     */
    KT_CHECK(after + 4096u >= before);
    KT_CHECK(before + 4096u >= after);

    teardown();
}

static void test_reserve_enospc(void)
{
    struct vfs_file file;
    k_u8_t buf[1024];
    int rc;
    int saw_enospc = 0;
    k_size_t written = 0;
    k_size_t guard = 0;
    const k_size_t limit = 256u * 1024u * 1024u;

    ktest_begin("ramfs: write past reserve answers ENOSPC and the machine lives");

    setup();
    memset(buf, 0xa5, sizeof(buf));

    rc = vfs_open(RAMFS_TEST_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        teardown();
        return;
    }

    while (written < limit && guard < (limit / sizeof(buf))) {
        int n = vfs_write(&file, buf, sizeof(buf));
        guard++;
        if (n == KERR_NOSPC) {
            saw_enospc = 1;
            break;
        }
        if (n < 0) {
            break;
        }
        if (n == 0) {
            break;
        }
        written += (k_size_t)n;
    }

    KT_CHECK_INT_EQ(saw_enospc, 1);
    KT_CHECK(written > 0);

    /* The machine is still alive: statfs, stat and close still work. */
    {
        struct vfs_statfs sfs;
        struct vfs_stat st;
        KT_CHECK_INT_EQ(vfs_statfs(RAMFS_POINT, &sfs), 0);
        KT_CHECK_INT_EQ(vfs_stat(RAMFS_TEST_FILE, &st), 0);
        KT_CHECK(st.size > 0);
    }

    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_unlink(RAMFS_TEST_FILE), 0);

    teardown();
}

static void test_df_stat(void)
{
    struct vfs_statfs sfs;
    struct vfs_stat st;
    struct vfs_statx stx;
    struct vfs_file file;
    int rc;

    ktest_begin("ramfs: df/stat report size and free space");

    setup();

    rc = vfs_statfs(RAMFS_POINT, &sfs);
    KT_CHECK_INT_EQ(rc, 0);
    KT_CHECK(sfs.block_size == 1024u);
    KT_CHECK(sfs.total_blocks > 0);
    KT_CHECK(sfs.free_blocks <= sfs.total_blocks);

    rc = vfs_open(RAMFS_TEST_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc == 0) {
        KT_CHECK_INT_EQ(vfs_write(&file, "hello", 5), 5);
        (void)vfs_close(&file);
    }

    KT_CHECK_INT_EQ(vfs_stat(RAMFS_TEST_FILE, &st), 0);
    KT_CHECK_INT_EQ((int)st.size, 5);
    KT_CHECK_INT_EQ(st.type, 1);

    KT_CHECK_INT_EQ(vfs_statx(RAMFS_TEST_FILE, &stx), 0);
    KT_CHECK_INT_EQ((int)stx.size, 5);

    /*
     * The stamp is the clock of the machine, whatever it says: a machine
     * without a battery has a clock that starts at zero and counts the uptime,
     * so "greater than zero" is a property of how long the test has run and not
     * of the filesystem. The stamp of the creation is now (or a second ago),
     * and the change is the same moment.
     */
    {
        k_u64_t now = k_clock_now_unix();

        KT_CHECK(stx.mtime <= now && stx.mtime + 2u >= now);
        KT_CHECK(stx.ctime == stx.mtime);
    }

    KT_CHECK_INT_EQ(vfs_unlink(RAMFS_TEST_FILE), 0);
    teardown();
}

static void test_unlink_open_file(void)
{
    struct vfs_file file;
    struct vfs_stat st;
    char buf[16];
    int rc;

    ktest_begin("ramfs: unlink of an open file keeps the blocks until close");

    setup();

    rc = vfs_open(RAMFS_TEST_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_write(&file, "data", 4), 4);
    KT_CHECK_INT_EQ(vfs_unlink(RAMFS_TEST_FILE), 0);

    /* The name is gone, but the open descriptor still reads the data. */
    KT_CHECK(vfs_stat(RAMFS_TEST_FILE, &st) != 0);

    file.offset = 0;
    memset(buf, 0, sizeof(buf));
    KT_CHECK_INT_EQ(vfs_read(&file, buf, 4), 4);
    KT_CHECK(memcmp(buf, "data", 4) == 0);

    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK(vfs_stat(RAMFS_TEST_FILE, &st) != 0);

    teardown();
}

static void test_read_past_end(void)
{
    struct vfs_file file;
    k_u8_t buf[32];
    int rc;

    ktest_begin("ramfs: read past the end of a file answers zeros");

    setup();

    rc = vfs_open(RAMFS_TEST_FILE, VFS_O_RDWR | VFS_O_CREAT | VFS_O_TRUNC, &file, g_file_data);
    KT_CHECK_INT_EQ(rc, 0);
    if (rc != 0) {
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(vfs_write(&file, "ab", 2), 2);

    memset(buf, 0xff, sizeof(buf));
    file.offset = 2;
    KT_CHECK_INT_EQ(vfs_read(&file, buf, 8), 8);
    KT_CHECK(buf[0] == 0 && buf[1] == 0 && buf[7] == 0);

    memset(buf, 0xff, sizeof(buf));
    file.offset = 100;
    KT_CHECK_INT_EQ(vfs_read(&file, buf, 4), 4);
    KT_CHECK(buf[0] == 0 && buf[3] == 0);

    KT_CHECK_INT_EQ(vfs_close(&file), 0);
    KT_CHECK_INT_EQ(vfs_unlink(RAMFS_TEST_FILE), 0);

    teardown();
}

void test_ramfs(void)
{
    /*
     * A process under a sanitizer does not report the memory of the machine:
     * the numbers of the C library are the numbers of the plain allocator, and
     * the one of the sanitizer is another. The filesystem would answer ENOSPC
     * to every write for a reason that is not its own, so the cases run where
     * the port can name its heap - the plain and the release builds - and say
     * so here instead of passing for the wrong reason.
     */
    if (k_sysinfo_heap_free() < (k_size_t)(RAMFS_RESERVE_KB + 2048u) * 1024u) {
        ktest_begin("ramfs: the port cannot name its memory under this build (skipped)");
        KT_CHECK(1);
        return;
    }

    test_write_read_8m();
    test_unlink_returns_heap();
    test_reserve_enospc();
    test_df_stat();
    test_unlink_open_file();
    test_read_past_end();
}
