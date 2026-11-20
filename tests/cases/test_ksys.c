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
static struct k_blockdev g_device;

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
        {KSYS_FREE, "free"},       {KSYS_REALLOC, "realloc"}, {KSYS_CHMOD, "chmod"},
        {KSYS_CHOWN, "chown"},     {KSYS_EXECVE, "execve"},
    };
    size_t i;

    ktest_begin("ksys: the numbers of the system calls do not change");

    setup();

    for (i = 0; i < K_ARRAY_SIZE(golden); i++) {
        KT_CHECK_STR_EQ(ksys_name(golden[i].number), golden[i].name);
        KT_CHECK_INT_EQ(golden[i].number, (k_u32_t)i);
    }

    /* Every call of the table is registered, and no number is left empty. */
    KT_CHECK_INT_EQ(ksys_count(), KSYS_COUNT);
    KT_CHECK(ksys_name(KSYS_COUNT) == NULL);

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
    struct vfs_stat info;
    int fd;

    ktest_begin("ksys: the errors do not depend on the way of the call");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);

    /* A file that does not exist. */
    KT_CHECK_INT_EQ(ksys_open("/nosuchfile", KSYS_O_RDONLY), KERR_NOENT);
    KT_CHECK_INT_EQ(ksys_stat("/nosuchfile", &info), KERR_NOENT);
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
 * The property: a descriptor remembers how it was opened. A file opened to
 * be read from cannot be written to, a file opened to be written to cannot
 * be read from - the same way through the system calls, no matter which
 * filesystem is under them.
 */
static void test_access_modes(void)
{
    char buffer[16];
    int rd;
    int wr;
    int rw;

    ktest_begin("ksys: a descriptor keeps the access it was opened with");

    setup();
    KT_CHECK_INT_EQ(load_corpus(), 0);
    KT_CHECK_INT_EQ(ksys_mkdir("/t"), 0);

    /* A file to work with: it exists and has some content. */
    wr = ksys_open("/t/perm.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(wr >= 0);
    if (wr >= 0) {
        KT_CHECK_INT_EQ(ksys_write(wr, g_text, 8), 8);
        KT_CHECK_INT_EQ(ksys_close(wr), 0);
    }

    /* Read only: reading works, writing is refused. */
    rd = ksys_open("/t/perm.txt", KSYS_O_RDONLY);
    KT_CHECK(rd >= 0);
    if (rd >= 0) {
        KT_CHECK(ksys_read(rd, buffer, sizeof(buffer)) >= 0);
        KT_CHECK_INT_EQ(ksys_write(rd, buffer, 1), KERR_BADF);
        KT_CHECK_INT_EQ(ksys_close(rd), 0);
    }

    /* Write only: writing works, reading is refused. */
    wr = ksys_open("/t/perm.txt", KSYS_O_WRONLY);
    KT_CHECK(wr >= 0);
    if (wr >= 0) {
        KT_CHECK_INT_EQ(ksys_write(wr, g_text, 8), 8);
        KT_CHECK_INT_EQ(ksys_read(wr, buffer, sizeof(buffer)), KERR_BADF);
        KT_CHECK_INT_EQ(ksys_close(wr), 0);
    }

    /* Read and write: both work. */
    rw = ksys_open("/t/perm.txt", KSYS_O_RDWR);
    KT_CHECK(rw >= 0);
    if (rw >= 0) {
        KT_CHECK(ksys_read(rw, buffer, sizeof(buffer)) >= 0);
        KT_CHECK(ksys_write(rw, buffer, 8) >= 0);
        KT_CHECK_INT_EQ(ksys_close(rw), 0);
    }

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

/*
 * The property: a path is checked at the edge of the system. NULL, the empty
 * string and a path that does not begin at the root name nothing, and a path
 * that does not end inside the window the VFS can hold is refused without the
 * kernel reading past that window.
 */
static void test_path_validation(void)
{
    /* Not one terminator inside the window: the kernel has to stop at the
     * end of the array, which is what the sanitizer build watches for. */
    static char unterminated[VFS_PATH_MAX];
    static char too_long[VFS_PATH_MAX + 1];
    static char longest[KSYS_PATH_MAX + 1];
    struct vfs_stat info;
    k_size_t used = 0;
    k_size_t i;

    ktest_begin("ksys: a path is checked at the edge");

    setup();

    /* NULL and the empty string are not paths. */
    KT_CHECK_INT_EQ(ksys_open(NULL, KSYS_O_RDONLY), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_open("", KSYS_O_RDONLY), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_unlink(NULL), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_mkdir(""), KERR_INVAL);

    /* Every filesystem hangs below the root: a relative path names nothing. */
    KT_CHECK_INT_EQ(ksys_open("etc/init.d", KSYS_O_RDONLY), KERR_INVAL);

    /* The raw interface answers the same way, NULL included. */
    KT_CHECK_INT_EQ(ksys_call(KSYS_OPEN, 0, KSYS_O_RDONLY, 0, 0, 0, 0), KERR_INVAL);

    memset(unterminated, 'a', sizeof(unterminated));
    unterminated[0] = '/';
    KT_CHECK_INT_EQ(ksys_open(unterminated, KSYS_O_RDONLY), KERR_TOOBIG);

    /* One byte more than the window, with the terminator outside of it. */
    memset(too_long, 'a', sizeof(too_long));
    too_long[0] = '/';
    too_long[sizeof(too_long) - 1] = '\0';
    KT_CHECK_INT_EQ(ksys_stat(too_long, &info), KERR_TOOBIG);

    /* The bound itself is still a path: 127 characters are accepted, and the
     * filesystem has the last word about what they name. */
    longest[used++] = '/';
    for (i = 0; i < 42; i++) {
        longest[used++] = 'a';
        longest[used++] = '/';
    }
    for (i = 0; i < 42; i++) {
        longest[used++] = 'b';
    }
    longest[used] = '\0';
    KT_CHECK_INT_EQ(used, KSYS_PATH_MAX);
    KT_CHECK_INT_EQ(ksys_open(longest, KSYS_O_RDONLY), KERR_NOENT);

    teardown();
}

/*
 * The property: the buffer of a call is checked at the edge too. NULL is
 * only meaningful with a zero length, a length the kernel never touches in
 * one call is refused, and the whence of the seek is one of three values.
 */
static void test_buffer_validation(void)
{
    char buffer[8];
    struct vfs_stat info;
    int fd;

    ktest_begin("ksys: a buffer and a length are checked at the edge");

    setup();

    fd = ksys_open("/edge.txt", KSYS_O_RDWR | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);

    if (fd >= 0) {
        /* NULL with a length is not a buffer. */
        KT_CHECK_INT_EQ(ksys_read(fd, NULL, 1), KERR_INVAL);
        KT_CHECK_INT_EQ(ksys_write(fd, NULL, 1), KERR_INVAL);

        /* The console of the system is behind the same door. */
        KT_CHECK_INT_EQ(ksys_write(1, NULL, 1), KERR_INVAL);

        /* A length no call of this system touches in one go. */
        KT_CHECK_INT_EQ(ksys_read(fd, buffer, KSYS_MAX_IO + 1), KERR_TOOBIG);
        KT_CHECK_INT_EQ(ksys_write(fd, buffer, KSYS_MAX_IO + 1), KERR_TOOBIG);

        /* A request of no bytes is not an error and reads nothing. */
        KT_CHECK_INT_EQ(ksys_write(fd, buffer, 0), 0);
        KT_CHECK_INT_EQ(ksys_read(fd, buffer, 0), 0);
        KT_CHECK_INT_EQ(ksys_read(fd, NULL, 0), 0);

        /* The whence is part of the signature: three values, no more. */
        KT_CHECK_INT_EQ(ksys_lseek(fd, 0, KSYS_SEEK_END + 1), KERR_INVAL);
        KT_CHECK_INT_EQ(ksys_lseek(fd, 0, -1), KERR_INVAL);

        /* A wide number must not be cut down to a valid whence by the cast
         * to int; the width of the arguments belongs to the target. */
        if (sizeof(ksys_arg_t) > sizeof(int)) {
            KT_CHECK_INT_EQ(
                ksys_call(KSYS_LSEEK, (ksys_arg_t)fd, 0, (ksys_arg_t)0x100000000ull, 0, 0, 0),
                KERR_INVAL);
        }

        /* stat and getcwd need the place for the answer. */
        KT_CHECK_INT_EQ(ksys_stat("/edge.txt", NULL), KERR_INVAL);
        KT_CHECK_INT_EQ(ksys_getcwd(NULL, 0), KERR_INVAL);
        KT_CHECK_INT_EQ(ksys_getcwd(buffer, 0), KERR_INVAL);

        /* The valid forms are unchanged. */
        KT_CHECK_INT_EQ(ksys_lseek(fd, 0, KSYS_SEEK_END), 0);
        KT_CHECK_INT_EQ(ksys_stat("/edge.txt", &info), 0);
        KT_CHECK_INT_EQ(info.type, 1); /* a file, not a directory */
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    teardown();
}

/*
 * The property: an open with flags outside the contract is refused at the
 * edge, before a descriptor is spent and before a filesystem is asked.
 */
static void test_open_flags(void)
{
    char data[1] = {'x'};
    int fd;

    ktest_begin("ksys: the flags of an open are the contract");

    setup();

    /* The bits that no flag owns. */
    KT_CHECK_INT_EQ(ksys_open("/flags.txt", KSYS_O_RDONLY | 0x0004), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_open("/flags.txt", KSYS_O_RDONLY | 0x8000), KERR_INVAL);

    /* Both access bits at once are not one of the three modes. */
    KT_CHECK_INT_EQ(ksys_open("/flags.txt", KSYS_O_ACCMODE), KERR_INVAL);

    /* The flags are checked first: the file does not exist, and the answer
     * is still about the flags. */
    KT_CHECK_INT_EQ(ksys_open("/no/such/file", KSYS_O_RDONLY | 0x8000), KERR_INVAL);

    /* Every flag of the contract still opens a real file. */
    fd = ksys_open("/flags.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC | KSYS_O_APPEND);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, data, sizeof(data)), 1);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A directory is a real object but is not an open file: the filesystem
     * keeps the last word about the object itself. */
    KT_CHECK_INT_EQ(ksys_open("/", KSYS_O_RDONLY), KERR_ISDIR);

    teardown();
}

/*
 * The property: chmod and chown change the numbers that stat reports, one at
 * a time, and a number the record of the filesystem cannot hold is refused
 * at the edge instead of being cut to fit.
 */
static void test_attributes(void)
{
    struct vfs_stat info;
    int fd;

    ktest_begin("ksys: chmod and chown change what stat reports");

    setup();

    fd = ksys_open("/attr.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    KT_CHECK_INT_EQ(ksys_stat("/attr.txt", &info), 0);
    KT_CHECK_INT_EQ(info.mode, MFS_MODE_DEFAULT);
    KT_CHECK_INT_EQ(info.owner, 0);

    KT_CHECK_INT_EQ(ksys_chmod("/attr.txt", 0755), 0);
    KT_CHECK_INT_EQ(ksys_stat("/attr.txt", &info), 0);
    KT_CHECK_INT_EQ(info.mode, 0755);

    KT_CHECK_INT_EQ(ksys_chown("/attr.txt", 42), 0);
    KT_CHECK_INT_EQ(ksys_stat("/attr.txt", &info), 0);
    KT_CHECK_INT_EQ(info.owner, 42);
    KT_CHECK_INT_EQ(info.mode, 0755); /* the two numbers are two fields */

    /* The numbers are the numbers of the record, not a window into it. */
    KT_CHECK_INT_EQ(ksys_chmod("/attr.txt", KSYS_MODE_MASK + 1u), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_chown("/attr.txt", KSYS_UID_MAX + 1u), KERR_INVAL);

    /* A zero mode would come back as the mode of the design: it is refused
     * where the record is written, and not stored as a number that lies. */
    KT_CHECK_INT_EQ(ksys_chmod("/attr.txt", 0), KERR_INVAL);

    /* The path is a path, and an object that is not there is not there. */
    KT_CHECK_INT_EQ(ksys_chmod(NULL, 0644), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_chown("relative", 0), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_chmod("/no-such-file", 0644), KERR_NOENT);

    /* The mode is still the one the last successful call wrote. */
    KT_CHECK_INT_EQ(ksys_stat("/attr.txt", &info), 0);
    KT_CHECK_INT_EQ(info.mode, 0755);
    KT_CHECK_INT_EQ(info.owner, 42);

    teardown();
}

/*
 * The property: the access of an open is checked against the mode of the
 * file, at the edge, before a descriptor exists. There is one user, uid 0,
 * and no bypass for it: the bits of the owner decide, and a file of the mode
 * 0200 cannot be read by anybody.
 */
static void test_open_mode(void)
{
    char byte = 'x';
    int fd;

    ktest_begin("ksys: the mode of a file decides what an open may ask for");

    setup();

    fd = ksys_open("/mode.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), 1);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* To be read only: an open that writes is refused, and the read still
     * works and the descriptor still refuses the write. */
    KT_CHECK_INT_EQ(ksys_chmod("/mode.txt", 0400), 0);
    KT_CHECK_INT_EQ(ksys_open("/mode.txt", KSYS_O_WRONLY), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_open("/mode.txt", KSYS_O_RDWR), KERR_PERM);

    fd = ksys_open("/mode.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_read(fd, &byte, 1), 1);
        KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), KERR_BADF);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* To be written only: the open to read does not reach the descriptor.
     * The owner is uid 0, the only user of the system, and it gets nothing
     * extra for being the owner. */
    KT_CHECK_INT_EQ(ksys_chmod("/mode.txt", 0200), 0);
    KT_CHECK_INT_EQ(ksys_chown("/mode.txt", 0), 0);
    KT_CHECK_INT_EQ(ksys_open("/mode.txt", KSYS_O_RDONLY), KERR_PERM);

    fd = ksys_open("/mode.txt", KSYS_O_WRONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A file of another owner is answered by the triad of the others: the
     * caller is not that owner, and there is no group of its own. */
    KT_CHECK_INT_EQ(ksys_chown("/mode.txt", 7), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/mode.txt", 0600), 0);
    KT_CHECK_INT_EQ(ksys_open("/mode.txt", KSYS_O_RDONLY), KERR_PERM);

    KT_CHECK_INT_EQ(ksys_chmod("/mode.txt", 0604), 0);
    fd = ksys_open("/mode.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A file that is about to be created is not checked: it is born with the
     * mode of the design, which allows the access that was asked for. */
    fd = ksys_open("/fresh.txt", KSYS_O_RDONLY | KSYS_O_CREAT);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    teardown();
}

/*
 * The property: readdir takes whole entries. A count that is not a multiple
 * of the size of an entry would make the filesystem write the last entry
 * past the end of the buffer of the caller.
 */
static void test_readdir_validation(void)
{
    struct vfs_dirent entry;
    int fd;

    ktest_begin("ksys: readdir takes whole entries");

    setup();

    /*
     * The arguments are checked before the filesystem is asked, so a file
     * of this directory is enough to reach the checks: a directory cannot
     * be opened through this ABI yet.
     */
    fd = ksys_open("/dir.txt", KSYS_O_RDONLY | KSYS_O_CREAT);
    KT_CHECK(fd >= 0);

    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_readdir(fd, NULL, sizeof(entry)), KERR_INVAL);
        KT_CHECK_INT_EQ(ksys_readdir(fd, &entry, sizeof(entry) - 1), KERR_INVAL);
        KT_CHECK_INT_EQ(ksys_readdir(fd, &entry, KSYS_MAX_IO + 1), KERR_TOOBIG);

        /* No space for an entry is not an error: there is nothing to list. */
        KT_CHECK_INT_EQ(ksys_readdir(fd, &entry, 0), 0);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* The descriptor is checked first, as for every call on a file. */
    KT_CHECK_INT_EQ(ksys_readdir(99, &entry, sizeof(entry)), KERR_BADF);

    teardown();
}

void test_ksys(void)
{
    test_abi_is_stable();
    test_write_read_roundtrip();
    test_alignment_invariance();
    test_error_codes();
    test_access_modes();
    test_syscalls_and_vfs_agree();
    test_values_survive();
    test_path_validation();
    test_buffer_validation();
    test_open_flags();
    test_attributes();
    test_open_mode();
    test_readdir_validation();
}
