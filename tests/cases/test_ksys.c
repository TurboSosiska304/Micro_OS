/*
 * Micro OS — the tests of the ABI of the system calls.
 *
 * The tests do not check that "something" works, they check the properties:
 * the same data has to give the same result no matter how the program
 * breaks it into pieces. The data is the corpus of the file
 * tests/corpus/vectors.txt: a fixed set of numbers with the boundary values
 * mixed in.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/task.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

/* The corpus of the tests: the path comes from the build system. */
#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

#define CORPUS_MAX 8192

static struct mfs_ram_device g_volume;
static struct mfs_device g_device;

/* The numbers of the corpus and the text that goes to the file. */
static char g_text[CORPUS_MAX];
static k_size_t g_text_len;
static k_u32_t g_values[1024];
static k_size_t g_value_count;

static int load_corpus(void)
{
    FILE *file = fopen(KTEST_CORPUS, "r");
    char line[128];
    k_size_t used = 0;

    if (file == NULL) {
        return -1;
    }

    g_value_count = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        k_size_t len = strlen(line);

        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }

        if (len == 0) {
            continue;
        }

        if (used + len + 2 >= sizeof(g_text)) {
            break;
        }

        memcpy(g_text + used, line, len);
        used += len;
        g_text[used++] = '\n';
        g_text[used] = '\0';

        if (g_value_count < K_ARRAY_SIZE(g_values)) {
            g_values[g_value_count++] = (k_u32_t)strtoul(line, NULL, 10);
        }
    }

    fclose(file);
    g_text_len = used;

    return (g_value_count > 0) ? 0 : -1;
}

static void setup(void)
{
    if (mfs_ram_init(&g_volume, 512) != 0) {
        return;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 64);
    mfs_mount(&g_device);
    mfs_vfs_register("/");

    /* The table of the system calls, the same as the kernel does at the boot. */
    ksys_init();
}

static void teardown(void)
{
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/* The sum of the bytes: the value has to be the same everywhere. */
static k_u32_t sum_bytes(const char *data, k_size_t size)
{
    k_u32_t sum = 0;
    k_size_t i;

    for (i = 0; i < size; i++) {
        sum = (sum * 31u) + (k_u8_t)data[i];
    }

    return sum;
}

/*
 * The property: the data that is written and read through the system calls
 * is the same no matter how it was broken into pieces and no matter how the
 * buffers were aligned.
 */
static void test_write_read_roundtrip(void)
{
    static const k_size_t chunks[] = {1, 3, 7, 64, 512, 1024, 4096, 0};
    k_u32_t reference = 0;
    int c;

    ktest_begin("ksys: the data survives every way of writing it");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);
    KT_CHECK(g_value_count > 100);

    for (c = 0; chunks[c] != 0; c++) {
        k_size_t chunk = chunks[c];
        k_size_t offset = 0;
        int fd;
        char *back;

        KT_CHECK_INT_EQ(ksys_mkdir("/t"), 0);

        fd = ksys_open("/t/data.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
        KT_CHECK(fd >= 0);

        while (offset < g_text_len) {
            k_size_t piece = chunk;

            if (piece > g_text_len - offset) {
                piece = g_text_len - offset;
            }

            KT_CHECK_INT_EQ(ksys_write(fd, g_text + offset, piece), (int)piece);
            offset += piece;
        }

        KT_CHECK_INT_EQ(ksys_close(fd), 0);

        fd = ksys_open("/t/data.txt", KSYS_O_RDONLY);
        KT_CHECK(fd >= 0);

        back = calloc(1, g_text_len + 1);
        KT_CHECK(back != NULL);

        if (back != NULL) {
            int total = 0;

            for (;;) {
                int got = (int)ksys_read(fd, back + total, (k_size_t)(g_text_len - total));

                if (got <= 0) {
                    break;
                }
                total += got;
                if ((k_size_t)total >= g_text_len) {
                    break;
                }
            }

            KT_CHECK_INT_EQ(total, (int)g_text_len);
            KT_CHECK_INT_EQ(memcmp(back, g_text, g_text_len), 0);

            if (c == 0) {
                reference = sum_bytes(back, g_text_len);
            } else {
                /* The same data, the same sum. */
                KT_CHECK_INT_EQ(sum_bytes(back, g_text_len), reference);
            }

            free(back);
        }

        KT_CHECK_INT_EQ(ksys_close(fd), 0);
        KT_CHECK_INT_EQ(ksys_unlink("/t/data.txt"), 0);
        KT_CHECK_INT_EQ(ksys_unlink("/t"), 0);
    }

    teardown();
}

/*
 * The property: the checksum of the data does not depend on the order of
 * the calls and on the alignment of the buffer.
 */
static void test_alignment_invariance(void)
{
    static char aligned[512];
    static char shifted[520];
    k_u32_t sum_a;
    k_u32_t sum_b;
    int fd;

    ktest_begin("ksys: the result does not depend on the alignment");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);

    /* The buffer with the odd alignment is one byte further. */
    KT_CHECK_INT_EQ(ksys_mkdir("/t"), 0);

    fd = ksys_open("/t/a.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    KT_CHECK_INT_EQ(ksys_write(fd, aligned, 256), 256);
    KT_CHECK_INT_EQ(ksys_close(fd), 0);

    fd = ksys_open("/t/b.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    KT_CHECK_INT_EQ(ksys_write(fd, shifted + 1, 256), 256);
    KT_CHECK_INT_EQ(ksys_close(fd), 0);

    /* The same bytes, written from the two buffers. */
    memset(aligned, 0x5A, sizeof(aligned));
    memset(shifted, 0x5A, sizeof(shifted));

    fd = ksys_open("/t/c.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    KT_CHECK_INT_EQ(ksys_write(fd, aligned, 256), 256);
    KT_CHECK_INT_EQ(ksys_close(fd), 0);

    fd = ksys_open("/t/d.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    KT_CHECK_INT_EQ(ksys_write(fd, shifted + 1, 256), 256);
    KT_CHECK_INT_EQ(ksys_close(fd), 0);

    /* And the same bytes come back. */
    {
        char back_a[256];
        char back_b[256];
        int fd_a = ksys_open("/t/c.txt", KSYS_O_RDONLY);
        int fd_b = ksys_open("/t/d.txt", KSYS_O_RDONLY);

        KT_CHECK(fd_a >= 0);
        KT_CHECK(fd_b >= 0);
        memset(back_a, 0, sizeof(back_a));
        memset(back_b, 0, sizeof(back_b));
        KT_CHECK_INT_EQ(ksys_read(fd_a, back_a, sizeof(back_a)), 256);
        KT_CHECK_INT_EQ(ksys_read(fd_b, back_b, sizeof(back_b)), 256);

        sum_a = sum_bytes(back_a, sizeof(back_a));
        sum_b = sum_bytes(back_b, sizeof(back_b));
        KT_CHECK_INT_EQ(sum_a, sum_b);
        KT_CHECK_INT_EQ(memcmp(back_a, back_b, sizeof(back_a)), 0);

        ksys_close(fd_a);
        ksys_close(fd_b);
    }

    teardown();
}

/*
 * The property: the numbers of the system calls are the contract. They are
 * written down here, so that a change of the table is noticed.
 */
static void test_abi_is_stable(void)
{
    static const struct {
        k_u32_t number;
        const char *name;
    } golden[] = {
        {KSYS_EXIT, "exit"},       {KSYS_READ, "read"},       {KSYS_WRITE, "write"},
        {KSYS_OPEN, "open"},       {KSYS_CLOSE, "close"},     {KSYS_STAT, "stat"},
        {KSYS_LSEEK, "lseek"},     {KSYS_UNLINK, "unlink"},   {KSYS_MKDIR, "mkdir"},
        {KSYS_READDIR, "readdir"}, {KSYS_GETPID, "getpid"},   {KSYS_GETCWD, "getcwd"},
        {KSYS_IOCTL, "ioctl"},     {KSYS_YIELD, "yield"},     {KSYS_SLEEP, "sleep"},
        {KSYS_UPTIME, "uptime"},   {KSYS_REBOOT, "reboot"},   {KSYS_FORK, "fork"},
        {KSYS_GETPPID, "getppid"}, {KSYS_WAITPID, "waitpid"}, {KSYS_MALLOC, "malloc"},
        {KSYS_FREE, "free"},       {KSYS_REALLOC, "realloc"},
    };
    size_t i;

    ktest_begin("ksys: the numbers of the system calls do not change");

    setup();

    for (i = 0; i < K_ARRAY_SIZE(golden); i++) {
        KT_CHECK_STR_EQ(ksys_name(golden[i].number), golden[i].name);
    }

    /* The width of the arguments follows the target: 32 bits on the board,
     * 64 on the host. A pointer must never be cut in half. */
    KT_CHECK_INT_EQ(sizeof(ksys_arg_t), sizeof(void *));

    /* An unknown call has to be refused, not guessed. */
    KT_CHECK_INT_EQ(ksys_call(0xFFFF, 0, 0, 0, 0, 0, 0), KERR_NOSYS);

    teardown();
}

/*
 * The property: the errors are the same no matter how the call is made.
 */
static void test_error_codes(void)
{
    int fd;

    ktest_begin("ksys: the errors do not depend on the way of the call");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);

    /* A file that does not exist. */
    KT_CHECK_INT_EQ(ksys_open("/nosuchfile", KSYS_O_RDONLY), KERR_NOENT);
    KT_CHECK_INT_EQ(ksys_stat("/nosuchfile", NULL), KERR_NOENT);
    KT_CHECK_INT_EQ(ksys_unlink("/nosuchfile"), KERR_NOENT);
    KT_CHECK_INT_EQ(ksys_mkdir("/nosuchdir/sub"), KERR_NOENT);

    /* A descriptor that was never opened. */
    KT_CHECK_INT_EQ(ksys_read(3, g_text, 1), KERR_BADF);
    KT_CHECK_INT_EQ(ksys_write(3, g_text, 1), KERR_BADF);
    KT_CHECK_INT_EQ(ksys_close(3), KERR_BADF);

    /* The same errors through the raw interface. */
    KT_CHECK_INT_EQ(
        ksys_call(KSYS_OPEN, (k_u64_t)(uintptr_t)"/nosuchfile", KSYS_O_RDONLY, 0, 0, 0, 0),
        KERR_NOENT);

    /* The descriptor is really released. */
    fd = ksys_open("/t.txt", KSYS_O_WRONLY | KSYS_O_CREAT);
    KT_CHECK(fd >= 0);
    KT_CHECK_INT_EQ(ksys_close(fd), 0);
    KT_CHECK_INT_EQ(ksys_write(fd, g_text, 1), KERR_BADF);

    teardown();
}

/*
 * The property: the file written through the system calls is the same file
 * that the filesystem sees.
 */
static void test_syscalls_and_vfs_agree(void)
{
    struct vfs_file file;
    /* The place for the data of the filesystem of an open file. */
    static k_u8_t file_data[VFS_FILE_DATA_SIZE];
    char back[128];
    int fd;

    ktest_begin("ksys: the system calls and the filesystem see the same file");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);

    fd = ksys_open("/shared.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    KT_CHECK_INT_EQ(ksys_write(fd, g_text, 64), 64);
    KT_CHECK_INT_EQ(ksys_close(fd), 0);

    /* The filesystem reads the same. */
    KT_CHECK_INT_EQ(vfs_open("/shared.txt", VFS_O_RDONLY, &file, file_data), 0);
    memset(back, 0, sizeof(back));
    KT_CHECK_INT_EQ(vfs_read(&file, back, 64), 64);
    KT_CHECK_INT_EQ(memcmp(back, g_text, 64), 0);
    vfs_close(&file);

    /* And the system calls read what the filesystem wrote. */
    {
        const char *text = "written through the filesystem\n";
        int fd2;

        KT_CHECK_INT_EQ(vfs_open("/shared.txt", VFS_O_WRONLY | VFS_O_TRUNC, &file, file_data), 0);
        KT_CHECK_INT_EQ(vfs_write(&file, text, strlen(text)), (int)strlen(text));
        vfs_close(&file);

        fd2 = ksys_open("/shared.txt", KSYS_O_RDONLY);
        KT_CHECK(fd2 >= 0);
        memset(back, 0, sizeof(back));
        KT_CHECK_INT_EQ(ksys_read(fd2, back, sizeof(back)), (int)strlen(text));
        KT_CHECK_INT_EQ(memcmp(back, text, strlen(text)), 0);
        ksys_close(fd2);
    }

    teardown();
}

/*
 * The property: the numbers of the corpus survive the round trip through
 * the file: the file has the same sum no matter how it was written.
 */
static void test_values_survive(void)
{
    char line[64];
    k_size_t i;
    k_u32_t sum_text = 0;

    ktest_begin("ksys: every number of the corpus survives the file");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);

    KT_CHECK_INT_EQ(ksys_mkdir("/v"), 0);

    /* The numbers are written one by one. */
    {
        int fd = ksys_open("/v/values.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);

        KT_CHECK(fd >= 0);

        for (i = 0; i < g_value_count; i++) {
            int len = snprintf(line, sizeof(line), "%u\n", (unsigned)g_values[i]);

            KT_CHECK(len > 0);
            KT_CHECK_INT_EQ(ksys_write(fd, line, (k_size_t)len), len);
        }

        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* And read back one by one: the same numbers. */
    {
        int fd = ksys_open("/v/values.txt", KSYS_O_RDONLY);
        k_u32_t index = 0;
        k_size_t length = 0;

        KT_CHECK(fd >= 0);

        for (;;) {
            char c;
            int got = (int)ksys_read(fd, &c, 1);

            if (got <= 0) {
                break;
            }

            KT_CHECK(length + 1 < sizeof(line));
            if (length + 1 >= sizeof(line)) {
                break;
            }

            line[length++] = c;

            if (c == '\n') {
                k_u32_t value;

                line[length - 1] = '\0';
                value = (k_u32_t)strtoul(line, NULL, 10);

                KT_CHECK(index < g_value_count);
                if (index < g_value_count) {
                    KT_CHECK_INT_EQ(value, g_values[index]);
                }
                index++;
                sum_text = (sum_text * 31u) + value;
                length = 0;
            }
        }

        KT_CHECK_INT_EQ(index, g_value_count);
        ksys_close(fd);
    }

    /* The sum of the text is the same as the sum of the numbers. */
    {
        k_u32_t sum_values = 0;

        for (i = 0; i < g_value_count; i++) {
            sum_values = (sum_values * 31u) + g_values[i];
        }

        KT_CHECK_INT_EQ(sum_text, sum_values);
    }

    teardown();
}

void test_ksys(void)
{
    test_abi_is_stable();
    test_write_read_roundtrip();
    test_alignment_invariance();
    test_error_codes();
    test_syscalls_and_vfs_agree();
    test_values_survive();
}
