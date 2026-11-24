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

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/config.h"
#include "kernel/errno.h"
#include "kernel/exec.h"
#include "kernel/kfile.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

/* The corpus of the tests: the paths come from the build system. */
#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif
#ifndef KTEST_ABI_GOLDEN
#define KTEST_ABI_GOLDEN "tests/corpus/abi.txt"
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
        KT_CHECK_INT_EQ(ksys_rmdir("/t"), 0);
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
        {KSYS_EXIT, "exit"},           {KSYS_READ, "read"},
        {KSYS_WRITE, "write"},         {KSYS_OPEN, "open"},
        {KSYS_CLOSE, "close"},         {KSYS_STAT, "stat"},
        {KSYS_LSEEK, "lseek"},         {KSYS_UNLINK, "unlink"},
        {KSYS_MKDIR, "mkdir"},         {KSYS_READDIR, "readdir"},
        {KSYS_GETPID, "getpid"},       {KSYS_GETCWD, "getcwd"},
        {KSYS_IOCTL, "ioctl"},         {KSYS_YIELD, "yield"},
        {KSYS_SLEEP, "sleep"},         {KSYS_UPTIME, "uptime"},
        {KSYS_REBOOT, "reboot"},       {KSYS_FORK, "fork"},
        {KSYS_GETPPID, "getppid"},     {KSYS_WAITPID, "waitpid"},
        {KSYS_MALLOC, "malloc"},       {KSYS_FREE, "free"},
        {KSYS_REALLOC, "realloc"},     {KSYS_CHMOD, "chmod"},
        {KSYS_CHOWN, "chown"},         {KSYS_EXECVE, "execve"},
        {KSYS_STATX, "statx"},         {KSYS_PIPE, "pipe"},
        {KSYS_DUP2, "dup2"},           {KSYS_KILL, "kill"},
        {KSYS_SIGNAL, "signal"},       {KSYS_SIGRETURN, "sigreturn"},
        {KSYS_GETLIMIT, "getlimit"},   {KSYS_SETLIMIT, "setlimit"},
        {KSYS_RENAME, "rename"},       {KSYS_FTRUNCATE, "ftruncate"},
        {KSYS_GETUID, "getuid"},       {KSYS_GETEUID, "geteuid"},
        {KSYS_GETGID, "getgid"},       {KSYS_SETUID, "setuid"},
        {KSYS_SETGID, "setgid"},       {KSYS_RMDIR, "rmdir"},
        {KSYS_FSTAT, "fstat"},         {KSYS_POLL, "poll"},
        {KSYS_MMAP, "mmap"},           {KSYS_MUNMAP, "munmap"},
        {KSYS_MSYNC, "msync"},         {KSYS_BRK, "brk"},
        {KSYS_CHDIR, "chdir"},         {KSYS_CLOCK_GETTIME, "clock_gettime"},
        {KSYS_GETRUSAGE, "getrusage"}, {KSYS_CHGRP, "chgrp"},
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
 * The recorded ABI (tests/corpus/abi.txt).
 *
 * The table above says what the numbers of the calls are; the file says what
 * the whole of the ABI is: the calls with their arity and result, the numbers
 * of the errors, the flags, the limits and the layout of every structure that
 * crosses the door. The two have to agree. A number that moves, a flag that is
 * reused or a field that is added in the middle of a structure fails a check
 * here until the file is changed with it, which is what makes a release of
 * this system a promise to the programs that were built for it.
 *
 * The file is the record and the test is its reader: the tables below name
 * every constant the file may contain, so a name the test does not know is as
 * much of a failure as a name the file forgot. The arity and the result are
 * documentation of a signature that the compiler checks through the
 * prototypes of kernel/ksys.h; everything else is compared with the running
 * kernel.
 */
#define ABI_NAME_MAX 32
#define ABI_KSYS_MAX 64
#define ABI_CONST_MAX 96
#define ABI_LAYOUT_MAX 160
#define ABI_LINE_MAX 160

struct abi_call {
    k_u32_t number;
    char name[ABI_NAME_MAX];
    int arity;
    char result[ABI_NAME_MAX];
};

struct abi_value {
    char name[ABI_NAME_MAX];
    long long value;
};

struct abi_layout {
    int width;
    int is_offset; /* the file says "offset" and not "size" */
    char object[ABI_NAME_MAX];
    char field[ABI_NAME_MAX];
    long long value;
};

static struct abi_call g_abi_calls[ABI_KSYS_MAX];
static k_size_t g_abi_call_count;
static struct abi_value g_abi_values[ABI_CONST_MAX];
static k_size_t g_abi_value_count;
static struct abi_layout g_abi_layouts[ABI_LAYOUT_MAX];
static k_size_t g_abi_layout_count;
static int g_abi_count_seen;
static long long g_abi_count;

/* One record of the layout of a structure, as the test knows it. */
struct abi_field {
    const char *name;
    k_size_t offset;
};

struct abi_object {
    const char *name;
    k_size_t size;
    const struct abi_field *fields;
    k_size_t field_count;
};

static const struct abi_field g_vfs_stat_fields[] = {
    {"inode", offsetof(struct vfs_stat, inode)}, {"type", offsetof(struct vfs_stat, type)},
    {"size", offsetof(struct vfs_stat, size)},   {"mode", offsetof(struct vfs_stat, mode)},
    {"owner", offsetof(struct vfs_stat, owner)}, {"group", offsetof(struct vfs_stat, group)},
};

static const struct abi_field g_vfs_statx_fields[] = {
    {"inode", offsetof(struct vfs_statx, inode)}, {"type", offsetof(struct vfs_statx, type)},
    {"size", offsetof(struct vfs_statx, size)},   {"mode", offsetof(struct vfs_statx, mode)},
    {"owner", offsetof(struct vfs_statx, owner)}, {"group", offsetof(struct vfs_statx, group)},
    {"mtime", offsetof(struct vfs_statx, mtime)}, {"ctime", offsetof(struct vfs_statx, ctime)},
};

static const struct abi_field g_vfs_dirent_fields[] = {
    {"name", offsetof(struct vfs_dirent, name)},
    {"inode", offsetof(struct vfs_dirent, inode)},
    {"type", offsetof(struct vfs_dirent, type)},
    {"size", offsetof(struct vfs_dirent, size)},
};

static const struct abi_field g_k_exec_image_fields[] = {
    {"data", offsetof(struct k_exec_image, data)},
    {"size", offsetof(struct k_exec_image, size)},
    {"arch", offsetof(struct k_exec_image, arch)},
    {"setuid", offsetof(struct k_exec_image, setuid)},
    {"entry", offsetof(struct k_exec_image, entry)},
    {"text_size", offsetof(struct k_exec_image, text_size)},
    {"data_size", offsetof(struct k_exec_image, data_size)},
    {"bss_size", offsetof(struct k_exec_image, bss_size)},
    {"envp", offsetof(struct k_exec_image, envp)},
    {"path", offsetof(struct k_exec_image, path)},
};

static const struct abi_field g_ksys_pollfd_fields[] = {
    {"fd", offsetof(struct ksys_pollfd, fd)},
    {"events", offsetof(struct ksys_pollfd, events)},
    {"revents", offsetof(struct ksys_pollfd, revents)},
};

static const struct abi_field g_ksys_timespec_fields[] = {
    {"tv_sec", offsetof(struct ksys_timespec, tv_sec)},
    {"tv_nsec", offsetof(struct ksys_timespec, tv_nsec)},
};

static const struct abi_field g_ksys_rusage_fields[] = {
    {"utime_ms", offsetof(struct ksys_rusage, utime_ms)},
    {"stime_ms", offsetof(struct ksys_rusage, stime_ms)},
};

static const struct abi_object g_abi_objects[] = {
    {"ksys_arg_t", sizeof(ksys_arg_t), NULL, 0},
    {"vfs_stat", sizeof(struct vfs_stat), g_vfs_stat_fields, K_ARRAY_SIZE(g_vfs_stat_fields)},
    {"vfs_statx", sizeof(struct vfs_statx), g_vfs_statx_fields, K_ARRAY_SIZE(g_vfs_statx_fields)},
    {"vfs_dirent", sizeof(struct vfs_dirent), g_vfs_dirent_fields,
     K_ARRAY_SIZE(g_vfs_dirent_fields)},
    {"k_exec_image", sizeof(struct k_exec_image), g_k_exec_image_fields,
     K_ARRAY_SIZE(g_k_exec_image_fields)},
    {"ksys_pollfd", sizeof(struct ksys_pollfd), g_ksys_pollfd_fields,
     K_ARRAY_SIZE(g_ksys_pollfd_fields)},
    {"ksys_timespec", sizeof(struct ksys_timespec), g_ksys_timespec_fields,
     K_ARRAY_SIZE(g_ksys_timespec_fields)},
    {"ksys_rusage", sizeof(struct ksys_rusage), g_ksys_rusage_fields,
     K_ARRAY_SIZE(g_ksys_rusage_fields)},
};

/*
 * The constants the record may name, with the value the running kernel has.
 * A constant of the ABI that is added to the kernel is added here and to the
 * file in the same change; one that is removed goes from both.
 */
static const struct abi_value g_abi_known_values[] = {
    {"KERR_NONE", KERR_NONE},
    {"KERR_PERM", KERR_PERM},
    {"KERR_NOENT", KERR_NOENT},
    {"KERR_SRCH", KERR_SRCH},
    {"KERR_EXIST", KERR_EXIST},
    {"KERR_INTR", KERR_INTR},
    {"KERR_IO", KERR_IO},
    {"KERR_TOOBIG", KERR_TOOBIG},
    {"KERR_NOEXEC", KERR_NOEXEC},
    {"KERR_BADF", KERR_BADF},
    {"KERR_CHILD", KERR_CHILD},
    {"KERR_AGAIN", KERR_AGAIN},
    {"KERR_NOMEM", KERR_NOMEM},
    {"KERR_FAULT", KERR_FAULT},
    {"KERR_XDEV", KERR_XDEV},
    {"KERR_BUSY", KERR_BUSY},
    {"KERR_NOSYS", KERR_NOSYS},
    {"KERR_NOTDIR", KERR_NOTDIR},
    {"KERR_ISDIR", KERR_ISDIR},
    {"KERR_INVAL", KERR_INVAL},
    {"KERR_NOSPC", KERR_NOSPC},
    {"KERR_SPIPE", KERR_SPIPE},
    {"KERR_PIPE", KERR_PIPE},
    {"KERR_NAMETOOLONG", KERR_NAMETOOLONG},
    {"KERR_NOTEMPTY", KERR_NOTEMPTY},
    {"KERR_NOTSUP", KERR_NOTSUP},
    {"KSYS_O_RDONLY", KSYS_O_RDONLY},
    {"KSYS_O_WRONLY", KSYS_O_WRONLY},
    {"KSYS_O_RDWR", KSYS_O_RDWR},
    {"KSYS_O_CREAT", KSYS_O_CREAT},
    {"KSYS_O_TRUNC", KSYS_O_TRUNC},
    {"KSYS_O_APPEND", KSYS_O_APPEND},
    {"KSYS_O_DIRECTORY", KSYS_O_DIRECTORY},
    {"KSYS_O_EXCL", KSYS_O_EXCL},
    {"KSYS_O_ACCMODE", KSYS_O_ACCMODE},
    {"KSYS_O_ALLOWED", KSYS_O_ALLOWED},
    {"KSYS_WNOHANG", KSYS_WNOHANG},
    {"KSYS_MODE_MASK", KSYS_MODE_MASK},
    {"KSYS_UID", KSYS_UID},
    {"KSYS_UID_MAX", KSYS_UID_MAX},
    {"KSYS_SEEK_SET", KSYS_SEEK_SET},
    {"KSYS_SEEK_CUR", KSYS_SEEK_CUR},
    {"KSYS_SEEK_END", KSYS_SEEK_END},
    {"KSYS_SIGINT", KSYS_SIGINT},
    {"KSYS_SIGPIPE", KSYS_SIGPIPE},
    {"KSYS_SIGTERM", KSYS_SIGTERM},
    {"KSYS_SIGCHLD", KSYS_SIGCHLD},
    {"KSYS_SIG_DFL", KSYS_SIG_DFL},
    {"KSYS_SIG_IGN", KSYS_SIG_IGN},
    {"KSYS_PATH_MAX", KSYS_PATH_MAX},
    {"KSYS_MAX_IO", KSYS_MAX_IO},
    {"KSYS_POLLIN", KSYS_POLLIN},
    {"KSYS_POLLOUT", KSYS_POLLOUT},
    {"KSYS_POLLERR", KSYS_POLLERR},
    {"KSYS_POLL_MAX", KSYS_POLL_MAX},
    {"KSYS_PROT_READ", KSYS_PROT_READ},
    {"KSYS_PROT_WRITE", KSYS_PROT_WRITE},
    {"KSYS_PROT_EXEC", KSYS_PROT_EXEC},
    {"KSYS_MAP_SHARED", KSYS_MAP_SHARED},
    {"KSYS_MAP_PRIVATE", KSYS_MAP_PRIVATE},
    {"KSYS_MAP_ANONYMOUS", KSYS_MAP_ANONYMOUS},
    {"KSYS_PAGE_SIZE", KSYS_PAGE_SIZE},
    {"KSYS_CLOCK_REALTIME", KSYS_CLOCK_REALTIME},
    {"KSYS_CLOCK_MONOTONIC", KSYS_CLOCK_MONOTONIC},
    {"K_EXEC_MAGIC", K_EXEC_MAGIC},
    {"K_EXEC_VERSION", K_EXEC_VERSION},
    {"K_EXEC_HEADER_SIZE", K_EXEC_HEADER_SIZE},
    {"K_EXEC_ARCH_X86_64", K_EXEC_ARCH_X86_64},
    {"K_EXEC_ARCH_XTENSA", K_EXEC_ARCH_XTENSA},
    {"K_EXEC_BSS_MAX", K_EXEC_BSS_MAX},
};

/*
 * A comparison of the golden test prints the name of the record: the number
 * of the line of the test says which table failed, and this says which
 * constant, which call or which field of it.
 */
static void abi_expect(const char *what, long long actual, long long expected)
{
    ktest_checks++;

    if (actual != expected) {
        ktest_fail(__FILE__, __LINE__, "%s: %lld, expected %lld", what, actual, expected);
    }
}

static void abi_expect_str(const char *what, const char *actual, const char *expected)
{
    ktest_checks++;

    if (actual == NULL || expected == NULL || strcmp(actual, expected) != 0) {
        ktest_fail(__FILE__, __LINE__, "%s: \"%s\", expected \"%s\"", what,
                   actual != NULL ? actual : "(null)", expected != NULL ? expected : "(null)");
    }
}

/* The words of a result of a call, which the file may use. */
static int abi_result_known(const char *word)
{
    static const char *const words[] = {
        "int", "ssize", "u32", "u64", "ptr", "void", "never", "resumed",
    };
    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(words); i++) {
        if (strcmp(word, words[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

/*
 * Read the record into the tables above. Every line is one of five shapes;
 * a line of another shape is a change of the file that the test has to learn,
 * so it is reported instead of being skipped.
 */
static int abi_load(void)
{
    FILE *file = fopen(KTEST_ABI_GOLDEN, "r");
    char line[ABI_LINE_MAX];
    int errors = 0;

    if (file == NULL) {
        return -1;
    }

    g_abi_call_count = 0;
    g_abi_value_count = 0;
    g_abi_layout_count = 0;
    g_abi_count_seen = 0;
    g_abi_count = -1;

    while (fgets(line, sizeof(line), file) != NULL) {
        char *comment = strchr(line, '#');
        char first[ABI_NAME_MAX] = {0};

        if (comment != NULL) {
            *comment = '\0';
        }

        if (sscanf(line, "%31s", first) != 1) {
            continue;
        }

        if (strcmp(first, "count") == 0) {
            if (sscanf(line, "count %lld", &g_abi_count) != 1) {
                errors++;
            }
            g_abi_count_seen++;
        } else if (strcmp(first, "ksys") == 0) {
            struct abi_call *call;

            if (g_abi_call_count >= K_ARRAY_SIZE(g_abi_calls)) {
                errors++;
                continue;
            }

            call = &g_abi_calls[g_abi_call_count];
            if (sscanf(line, "ksys %u %31s %d %31s", &call->number, call->name, &call->arity,
                       call->result) != 4) {
                errors++;
                continue;
            }
            g_abi_call_count++;
        } else if (strcmp(first, "const") == 0) {
            struct abi_value *value;

            if (g_abi_value_count >= K_ARRAY_SIZE(g_abi_values)) {
                errors++;
                continue;
            }

            value = &g_abi_values[g_abi_value_count];
            if (sscanf(line, "const %31s %lld", value->name, &value->value) != 2) {
                errors++;
                continue;
            }
            g_abi_value_count++;
        } else if (strcmp(first, "size") == 0 || strcmp(first, "offset") == 0) {
            struct abi_layout *layout;

            if (g_abi_layout_count >= K_ARRAY_SIZE(g_abi_layouts)) {
                errors++;
                continue;
            }

            layout = &g_abi_layouts[g_abi_layout_count];
            layout->is_offset = (strcmp(first, "offset") == 0);
            layout->field[0] = '\0';

            if (layout->is_offset) {
                if (sscanf(line, "offset %d %31s %31s %lld", &layout->width, layout->object,
                           layout->field, &layout->value) != 4) {
                    errors++;
                    continue;
                }
            } else {
                if (sscanf(line, "size %d %31s %lld", &layout->width, layout->object,
                           &layout->value) != 3) {
                    errors++;
                    continue;
                }
            }
            g_abi_layout_count++;
        } else {
            /* A shape the test does not read: the file and the reader have to
             * grow together. */
            errors++;
        }
    }

    fclose(file);

    return (errors == 0 && g_abi_call_count > 0 && g_abi_count_seen == 1) ? 0 : -1;
}

static const struct abi_value *abi_known_value(const char *name)
{
    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(g_abi_known_values); i++) {
        if (strcmp(g_abi_known_values[i].name, name) == 0) {
            return &g_abi_known_values[i];
        }
    }

    return NULL;
}

static const struct abi_object *abi_object(const char *name)
{
    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(g_abi_objects); i++) {
        if (strcmp(g_abi_objects[i].name, name) == 0) {
            return &g_abi_objects[i];
        }
    }

    return NULL;
}

static const struct abi_field *abi_field(const struct abi_object *object, const char *name)
{
    k_size_t i;

    for (i = 0; i < object->field_count; i++) {
        if (strcmp(object->fields[i].name, name) == 0) {
            return &object->fields[i];
        }
    }

    return NULL;
}

/* The calls of the record: number, name, arity and result, against the table
 * of the kernel and against the table above. */
static void abi_check_calls(void)
{
    k_u32_t number;
    k_size_t i;

    abi_expect("the number of the calls", g_abi_count, KSYS_COUNT);
    abi_expect("ksys_count()", ksys_count(), KSYS_COUNT);

    for (i = 0; i < g_abi_call_count; i++) {
        const struct abi_call *call = &g_abi_calls[i];

        abi_expect("the number of a recorded call", call->number, i);
        if (call->number >= KSYS_COUNT) {
            continue;
        }

        abi_expect_str(call->name, ksys_name(call->number), call->name);
        abi_expect(call->name, call->arity >= 0 && call->arity <= 6, 1);
        abi_expect(call->name, abi_result_known(call->result), 1);
    }

    /* Every number is recorded exactly once: a call the file forgot, or two
     * records of one number, is a failure and not a gap to be filled. */
    for (number = 0; number < KSYS_COUNT; number++) {
        k_size_t seen = 0;

        for (i = 0; i < g_abi_call_count; i++) {
            if (g_abi_calls[i].number == number) {
                seen++;
            }
        }

        abi_expect(ksys_name(number), seen, 1);
    }
}

/* The constants: every record of the file is a name the test knows, and every
 * name the test knows is in the file. */
static void abi_check_values(void)
{
    k_size_t i;
    k_size_t j;

    for (i = 0; i < g_abi_value_count; i++) {
        const struct abi_value *value = &g_abi_values[i];
        const struct abi_value *known = abi_known_value(value->name);

        if (known == NULL) {
            ktest_checks++;
            ktest_fail(__FILE__, __LINE__, "%s: the name is not of the ABI of the test",
                       value->name);
            continue;
        }

        abi_expect(value->name, value->value, known->value);
    }

    for (i = 0; i < K_ARRAY_SIZE(g_abi_known_values); i++) {
        k_size_t seen = 0;

        for (j = 0; j < g_abi_value_count; j++) {
            if (strcmp(g_abi_values[j].name, g_abi_known_values[i].name) == 0) {
                seen++;
            }
        }

        abi_expect(g_abi_known_values[i].name, seen, 1);
    }
}

/* The layout: the record of the width of this machine is compared with the
 * structures, and the record of the other width has to be there too - the
 * file is the record of the board as well as of the host. */
static void abi_check_layout(void)
{
    const int width = (int)sizeof(void *);
    const int other = (width == 8) ? 4 : 8;
    k_size_t i;

    for (i = 0; i < g_abi_layout_count; i++) {
        const struct abi_layout *layout = &g_abi_layouts[i];
        const struct abi_object *object;

        /* The record names the two machines of the ABI, and nothing else. */
        if (layout->width != width && layout->width != other) {
            ktest_checks++;
            ktest_fail(__FILE__, __LINE__, "%s: the width %d is not a machine of the ABI",
                       layout->object, layout->width);
            continue;
        }

        object = abi_object(layout->object);
        if (object == NULL) {
            ktest_checks++;
            ktest_fail(__FILE__, __LINE__, "%s: the name is not of the ABI of the test",
                       layout->object);
            continue;
        }

        if (layout->is_offset && abi_field(object, layout->field) == NULL) {
            ktest_checks++;
            ktest_fail(__FILE__, __LINE__, "%s.%s: the field is not of the structure", object->name,
                       layout->field);
            continue;
        }

        /* The names of the other machine are checked as well - a record of the
         * board with a misspelt field is a record nobody can follow - but its
         * numbers belong to the build of the board, not to this one. */
        if (layout->width != width) {
            continue;
        }

        if (!layout->is_offset) {
            abi_expect(object->name, layout->value, (long long)object->size);
            continue;
        }

        abi_expect(layout->field, layout->value,
                   (long long)abi_field(object, layout->field)->offset);
    }

    /* Every structure and every field of it is recorded, on both machines. */
    for (i = 0; i < K_ARRAY_SIZE(g_abi_objects); i++) {
        const struct abi_object *object = &g_abi_objects[i];
        k_size_t j;
        int sized = 0;
        int sized_other = 0;

        for (j = 0; j < g_abi_layout_count; j++) {
            const struct abi_layout *layout = &g_abi_layouts[j];

            if (layout->is_offset || strcmp(layout->object, object->name) != 0) {
                continue;
            }

            sized += (layout->width == width);
            sized_other += (layout->width == other);
        }

        abi_expect(object->name, sized, 1);
        abi_expect(object->name, sized_other, 1);

        for (j = 0; j < object->field_count; j++) {
            k_size_t k;
            int seen = 0;

            for (k = 0; k < g_abi_layout_count; k++) {
                const struct abi_layout *layout = &g_abi_layouts[k];

                if (!layout->is_offset || layout->width != width) {
                    continue;
                }

                if (strcmp(layout->object, object->name) == 0 &&
                    strcmp(layout->field, object->fields[j].name) == 0) {
                    seen++;
                }
            }

            abi_expect(object->fields[j].name, seen, 1);
        }
    }
}

/*
 * The property: the ABI is the one that was recorded for 1.0. A change of a
 * number, a name, a flag or a layout without a line of tests/corpus/abi.txt
 * is a failure of this test, and a line of the file without a change of the
 * kernel is one too.
 */
static void test_abi_golden(void)
{
    ktest_begin("ksys: the ABI is the recorded one");

    setup();

    if (abi_load() != 0) {
        ktest_checks++;
        ktest_fail(__FILE__, __LINE__, "%s: cannot be read", KTEST_ABI_GOLDEN);
        teardown();
        return;
    }

    abi_check_calls();
    abi_check_values();
    abi_check_layout();

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
    KT_CHECK_INT_EQ(ksys_open(unterminated, KSYS_O_RDONLY), KERR_NAMETOOLONG);

    /* One byte more than the window, with the terminator outside of it. */
    memset(too_long, 'a', sizeof(too_long));
    too_long[0] = '/';
    too_long[sizeof(too_long) - 1] = '\0';
    KT_CHECK_INT_EQ(ksys_stat(too_long, &info), KERR_NAMETOOLONG);

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

#if KERNEL_EXEC
        /*
         * The vector of a program is copied at the edge as well: one word
         * more than the frame holds is E2BIG before any file is looked at.
         * (The addresses that are not memory of the caller are EFAULT only
         * where a port has an address space to check; that half is
         * programs/badptr.c and make x86-smoke.)
         */
        {
            static const char word[] = "x";
            char *many[16];
            k_size_t i;

            for (i = 0; i < 16u; i++) {
                many[i] = (char *)word;
            }

            KT_CHECK_INT_EQ(ksys_execve("/edge.txt", many), KERR_TOOBIG);
        }
#endif

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
 * The property: statx carries the numbers of stat and the two moments of a
 * change, the old stat keeps its structure and its answer, and without an
 * object or a place for the answer the call is the error of the edge.
 */
static void test_statx(void)
{
    struct vfs_stat plain;
    struct vfs_statx info;
    int fd;

    ktest_begin("ksys: statx carries the moments that stat has no field for");

    setup();

    fd = ksys_open("/statx.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, "abc", 3), 3);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    memset(&plain, 0, sizeof(plain));
    memset(&info, 0, sizeof(info));
    KT_CHECK_INT_EQ(ksys_stat("/statx.txt", &plain), 0);
    KT_CHECK_INT_EQ(ksys_statx("/statx.txt", &info), 0);

    /* The old call is untouched: the same object, the same numbers. */
    KT_CHECK_INT_EQ(info.inode, plain.inode);
    KT_CHECK_INT_EQ(info.type, plain.type);
    KT_CHECK_INT_EQ(info.mode, plain.mode);
    KT_CHECK_INT_EQ(info.owner, plain.owner);
    KT_CHECK_INT_EQ(info.size, (k_u64_t)3);

    /* A create and a write stamp the record; the clock has a moment even
     * when the machine has no RTC (the seconds since the boot). */
    KT_CHECK(info.ctime >= info.mtime);
    KT_CHECK_INT_EQ(info.type, 1);

    KT_CHECK_INT_EQ(ksys_mkdir("/statx-dir"), 0);
    KT_CHECK_INT_EQ(ksys_statx("/statx-dir", &info), 0);
    KT_CHECK_INT_EQ(info.type, 2);

    /* The edge: an object that is not there and no place for the answer. */
    KT_CHECK_INT_EQ(ksys_statx("/no-such-object", &info), KERR_NOENT);
    KT_CHECK_INT_EQ(ksys_statx("/statx.txt", NULL), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_statx(NULL, &info), KERR_INVAL);

    teardown();
}

/*
 * The property: the access of an open is checked against the mode of the
 * file, at the edge, before a descriptor exists. The test runner is root,
 * and root passes the read and the write triads of every object; the check
 * of a file of another owner by a task that is not root is in `test_users`.
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

    /* Read only, and the caller is root: the write is allowed by the bypass.
     * A descriptor opened to read still refuses the write. */
    KT_CHECK_INT_EQ(ksys_chmod("/mode.txt", 0400), 0);
    fd = ksys_open("/mode.txt", KSYS_O_WRONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    fd = ksys_open("/mode.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_read(fd, &byte, 1), 1);
        KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), KERR_BADF);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* Write only: root reads it anyway. */
    KT_CHECK_INT_EQ(ksys_chmod("/mode.txt", 0200), 0);
    KT_CHECK_INT_EQ(ksys_chown("/mode.txt", 0), 0);
    fd = ksys_open("/mode.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A file of another owner is still read and written by root: the bypass
     * covers the three triads and does not look at the owner. */
    KT_CHECK_INT_EQ(ksys_chown("/mode.txt", 7), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/mode.txt", 0600), 0);
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
 * past the end of the buffer of the caller, and a descriptor that is not a
 * directory has no entries to give.
 */
static void test_readdir_validation(void)
{
    struct vfs_dirent entry;
    int fd;

    ktest_begin("ksys: readdir takes whole entries");

    setup();

    /*
     * The arguments are checked before the filesystem is asked, so a file
     * of this directory is enough to reach the checks.
     */
    fd = ksys_open("/dir.txt", KSYS_O_RDONLY | KSYS_O_CREAT);
    KT_CHECK(fd >= 0);

    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_readdir(fd, NULL, sizeof(entry)), KERR_INVAL);
        KT_CHECK_INT_EQ(ksys_readdir(fd, &entry, sizeof(entry) - 1), KERR_INVAL);
        KT_CHECK_INT_EQ(ksys_readdir(fd, &entry, KSYS_MAX_IO + 1), KERR_TOOBIG);

        /* No space for an entry is not an error: there is nothing to list. */
        KT_CHECK_INT_EQ(ksys_readdir(fd, &entry, 0), 0);

        /* A file is not a directory: the descriptor has no entries. */
        KT_CHECK_INT_EQ(ksys_readdir(fd, &entry, sizeof(entry)), KERR_NOTDIR);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* The descriptor is checked first, as for every call on a file. */
    KT_CHECK_INT_EQ(ksys_readdir(99, &entry, sizeof(entry)), KERR_BADF);

    teardown();
}

/*
 * The property: a directory of the path that the caller may not search
 * through keeps its children out of reach. The check is the `x` bit of every
 * directory on the way, and the object at the end has its own check for the
 * access that is asked of it. The runner is root, which the search does not
 * stop, so the walking caller is a task of uid 7 that owns the directory.
 */
static void test_directory_search(void)
{
    struct task *self = task_self();
    struct vfs_stat info;
    char byte = 'x';
    int fd;

    ktest_begin("ksys: the x bit of every directory decides the search");

    setup();

    KT_CHECK_INT_EQ(ksys_mkdir("/locked"), 0);
    fd = ksys_open("/locked/file.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, &byte, 1), 1);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A one-byte file is not a program, but the directory is asked first. */
    KT_CHECK_INT_EQ(ksys_chmod("/locked/file.txt", 0755), 0);

    /*
     * Read and write for the owner but no search: the child is out of reach
     * for every call that has to pass the directory. Root passes anyway, so
     * the walk is done as the owner of the directory, uid 7.
     */
    KT_CHECK_INT_EQ(ksys_chown("/locked", 7), 0);
    KT_CHECK_INT_EQ(ksys_chmod("/locked", 0600), 0);

    task_set_uid(self, 7);
    KT_CHECK_INT_EQ(ksys_open("/locked/file.txt", KSYS_O_RDONLY), KERR_PERM);
    KT_CHECK_INT_EQ(ksys_stat("/locked/file.txt", &info), KERR_PERM);
#if KERNEL_EXEC
    KT_CHECK_INT_EQ(ksys_execve("/locked/file.txt", NULL), KERR_PERM);
#endif

    /* The directory itself is still described: the end of the path is not
     * a directory that is passed through. */
    KT_CHECK_INT_EQ(ksys_stat("/locked", &info), 0);
    fd = ksys_open("/locked", KSYS_O_RDONLY | KSYS_O_DIRECTORY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* Search for the owner: the child comes back. */
    KT_CHECK_INT_EQ(ksys_chmod("/locked", 0700), 0);
    fd = ksys_open("/locked/file.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_stat("/locked/file.txt", &info), 0);
#if KERNEL_EXEC
    /* The file is still not an image of a program: the walk let it through
     * and the file itself is the next reason. */
    KT_CHECK_INT_EQ(ksys_execve("/locked/file.txt", NULL), KERR_INVAL);
#endif

    /* Search without read: the path is walked, but the directory cannot be
     * opened to list it. */
    KT_CHECK_INT_EQ(ksys_chmod("/locked", 0100), 0);
    KT_CHECK_INT_EQ(ksys_stat("/locked/file.txt", &info), 0);
    KT_CHECK_INT_EQ(ksys_open("/locked", KSYS_O_RDONLY | KSYS_O_DIRECTORY), KERR_PERM);

    /* The test runner is root again for the teardown. */
    task_set_uid(self, 0);
    teardown();
}

/*
 * The property: readdir reads the entries of a directory that was opened
 * with O_DIRECTORY, the position lives in the descriptor, and the end of
 * the directory is an empty answer and not an error.
 */
static void test_readdir_directory(void)
{
    struct vfs_dirent entries[8];
    k_ssize_t got;
    k_size_t i;
    int saw_etc = 0;
    char byte = 'x';
    int fd;

    ktest_begin("ksys: readdir reads the entries of an open directory");

    setup();

    KT_CHECK_INT_EQ(ksys_mkdir("/etc"), 0);

    fd = ksys_open("/", KSYS_O_RDONLY | KSYS_O_DIRECTORY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        got = ksys_readdir(fd, entries, sizeof(entries));
        KT_CHECK(got > 0);
        if (got > 0) {
            k_size_t count = (k_size_t)got / sizeof(struct vfs_dirent);

            for (i = 0; i < count; i++) {
                KT_CHECK(entries[i].name[0] != '\0');
                if (strcmp(entries[i].name, "etc") == 0 && entries[i].type == 2) {
                    saw_etc = 1;
                }
            }
        }
        KT_CHECK(saw_etc);

        /* The position belongs to the descriptor: the next calls go on,
         * and the end of the directory is an empty answer. */
        while (ksys_readdir(fd, entries, sizeof(entries)) > 0) {
        }
        KT_CHECK_INT_EQ(ksys_readdir(fd, entries, sizeof(entries)), 0);

        /* A directory is read by readdir, not as a stream of bytes. */
        KT_CHECK_INT_EQ(ksys_read(fd, &byte, 1), KERR_ISDIR);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* O_DIRECTORY goes with O_RDONLY and with nothing else. */
    KT_CHECK_INT_EQ(ksys_open("/", KSYS_O_RDONLY | KSYS_O_DIRECTORY | KSYS_O_WRONLY), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_open("/", KSYS_O_RDWR | KSYS_O_DIRECTORY), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_open("/", KSYS_O_RDONLY | KSYS_O_DIRECTORY | KSYS_O_TRUNC), KERR_INVAL);

    /* A file is not a directory, whatever the flag of the open says. */
    fd = ksys_open("/plain.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_open("/plain.txt", KSYS_O_RDONLY | KSYS_O_DIRECTORY), KERR_NOTDIR);

    teardown();
}

/*
 * The property: the two ends of a pipe are descriptors of the task, the
 * bytes come back in the order they went in, and the end of the writing end
 * is the end of the file.
 */
static void test_pipe(void)
{
    char out[32];
    char in[8];
    int fds[2];

    ktest_begin("ksys: a pipe carries the bytes from the writer to the reader");

    setup();

    KT_CHECK_INT_EQ(ksys_pipe(fds), 0);
    KT_CHECK(fds[0] >= 0 && fds[0] < TASK_MAX_FDS);
    KT_CHECK(fds[1] >= 0 && fds[1] < TASK_MAX_FDS);
    KT_CHECK(fds[0] != fds[1]);

    KT_CHECK_INT_EQ(ksys_write(fds[1], "hello", 5), 5);
    memset(out, 0, sizeof(out));
    KT_CHECK_INT_EQ(ksys_read(fds[0], out, sizeof(out)), 5);
    KT_CHECK_STR_EQ(out, "hello");

    /* A pipe has no position and no entries. */
    KT_CHECK_INT_EQ(ksys_lseek(fds[0], 0, KSYS_SEEK_SET), KERR_SPIPE);
    KT_CHECK_INT_EQ(ksys_readdir(fds[0], out, sizeof(struct vfs_dirent)), KERR_NOTDIR);

    /* Each end remembers its access: the reader cannot write and the writer
     * cannot read. */
    KT_CHECK_INT_EQ(ksys_write(fds[0], "x", 1), KERR_BADF);
    KT_CHECK_INT_EQ(ksys_read(fds[1], in, 1), KERR_BADF);

    /* The end of the writing end is the end of the file. */
    KT_CHECK_INT_EQ(ksys_close(fds[1]), 0);
    KT_CHECK_INT_EQ(ksys_read(fds[0], out, sizeof(out)), 0);
    KT_CHECK_INT_EQ(ksys_close(fds[0]), 0);

    /* A pipe whose reading end is closed is EPIPE: there are no signals in
     * this system, so the error is the whole answer. */
    KT_CHECK_INT_EQ(ksys_pipe(fds), 0);
    KT_CHECK_INT_EQ(ksys_close(fds[0]), 0);
    KT_CHECK_INT_EQ(ksys_write(fds[1], "lost", 4), KERR_PIPE);
    KT_CHECK_INT_EQ(ksys_close(fds[1]), 0);

    teardown();
}

/*
 * The grain of a pipe: the ring of the port is KFILE_PIPE_SIZE bytes and a
 * write of that size is one call and one step of the ring, not a chain of
 * 256-byte turns. The host builds the kernel with the 64 KiB ring of the
 * machine; the board has 4 KiB and the number is the one of its own header.
 */
static char g_pipe_block_in[KFILE_PIPE_SIZE];
static char g_pipe_block_out[KFILE_PIPE_SIZE];

static void test_pipe_block(void)
{
    k_size_t i;
    int fds[2];

    ktest_begin("ksys: a pipe carries a block of its ring in one write and one read");

    setup();

    for (i = 0; i < KFILE_PIPE_SIZE; i++) {
        g_pipe_block_in[i] = (char)(unsigned char)(i * 7u + 3u);
    }

    KT_CHECK_INT_EQ(ksys_pipe(fds), 0);
    KT_CHECK_INT_EQ(ksys_write(fds[1], g_pipe_block_in, KFILE_PIPE_SIZE),
                    (k_ssize_t)KFILE_PIPE_SIZE);

    memset(g_pipe_block_out, 0, sizeof(g_pipe_block_out));
    KT_CHECK_INT_EQ(ksys_read(fds[0], g_pipe_block_out, sizeof(g_pipe_block_out)),
                    (k_ssize_t)sizeof(g_pipe_block_out));
    KT_CHECK(memcmp(g_pipe_block_in, g_pipe_block_out, KFILE_PIPE_SIZE) == 0);

    /* The ring is full after that write: the next byte waits for the reader
     * and the bytes come back in the order they went in. */
    KT_CHECK_INT_EQ(ksys_close(fds[0]), 0);
    KT_CHECK_INT_EQ(ksys_close(fds[1]), 0);

    teardown();
}

/* --- the memory of a program --- */

/*
 * The property: the calls of the memory of a program check their arguments
 * before they look for a machine that has pages, and a machine without them
 * answers ENOSYS - the same answer it gives to execve. The host and the board
 * are such machines: a program there is a task of the kernel and shares its
 * memory, so there is no window to hand out page by page. The pages
 * themselves are covered on the machine that has them (`make x86-smoke`,
 * /bin/mmap and /bin/poll).
 */
static void test_mmap_edges(void)
{
    static _Alignas(4096) char page[8];

    ktest_begin("ksys: the memory of a program checks its arguments and says no without pages");

    setup();

    /* The arguments of a mapping: a length, a protection, one sharing mode,
     * an aligned address and an aligned offset. */
    KT_CHECK_INT_EQ(
        ksys_mmap(NULL, 0, KSYS_PROT_READ, KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0),
        KERR_INVAL);
    KT_CHECK_INT_EQ(
        ksys_mmap(NULL, 100, KSYS_PROT_READ, KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0),
        KERR_INVAL);
    KT_CHECK_INT_EQ(
        ksys_mmap(NULL, KSYS_PAGE_SIZE, 0x40, KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0),
        KERR_INVAL);
    KT_CHECK_INT_EQ(
        ksys_mmap(NULL, KSYS_PAGE_SIZE, KSYS_PROT_READ, KSYS_MAP_PRIVATE | KSYS_MAP_SHARED, -1, 0),
        KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_mmap(NULL, KSYS_PAGE_SIZE, KSYS_PROT_READ, 0, -1, 0), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_mmap(page + 1, KSYS_PAGE_SIZE, KSYS_PROT_READ,
                              KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0),
                    KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_mmap(NULL, KSYS_PAGE_SIZE, KSYS_PROT_READ,
                              KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 1),
                    KERR_INVAL);

    /* A machine without pages: the call is refused after the arguments. */
    KT_CHECK_INT_EQ(ksys_mmap(NULL, KSYS_PAGE_SIZE, KSYS_PROT_READ,
                              KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0),
                    KERR_NOSYS);
    KT_CHECK_INT_EQ(ksys_munmap(page, KSYS_PAGE_SIZE), KERR_NOSYS);
    KT_CHECK_INT_EQ(ksys_msync(page, KSYS_PAGE_SIZE), KERR_NOSYS);
    KT_CHECK_INT_EQ(ksys_brk(page), KERR_NOSYS);

    /* The release and the write-back have the same edge. */
    KT_CHECK_INT_EQ(ksys_munmap(page + 1, KSYS_PAGE_SIZE), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_munmap(page, 0), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_munmap(page, 100), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_msync(page + 1, KSYS_PAGE_SIZE), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_msync(page, 100), KERR_INVAL);

    teardown();
}

/* --- poll --- */

/*
 * The task that changes the state a `poll` waits for. It is a fork of the
 * test task, so the numbers of the descriptors of the test are its own: a
 * pipe is one object and two tasks that share the table of the parent share
 * its ends.
 */
struct poll_helper {
    int write_fd;   /* the end to write a byte to, or -1 */
    int signal_pid; /* the task to signal, or 0 */
    int signal;
    k_u32_t delay_ms;
};

static struct poll_helper g_poll_helper;

static void poll_helper_task(void *arg)
{
    struct poll_helper *helper = (struct poll_helper *)arg;

    ksys_sleep((ksys_arg_t)helper->delay_ms);

    if (helper->write_fd >= 0) {
        (void)ksys_write(helper->write_fd, "x", 1);
    }

    if (helper->signal_pid != 0) {
        (void)ksys_kill(helper->signal_pid, helper->signal);
    }
}

static void poll_ignore_signal(int sig)
{
    K_UNUSED(sig);
}

/*
 * The property: `poll` answers the state of the objects and waits in the
 * scheduler. The test covers the three answers a wait can have - a state
 * that is already true, a state that becomes true while the task waits, and
 * a deadline that passes - and the sleep of a list of no descriptor.
 */
static void test_poll(void)
{
    struct ksys_pollfd list[3];
    struct task *helper;
    char byte = 0;
    int a[2];
    int b[2];
    int empty[2];
    k_u64_t start;
    k_u64_t elapsed;

    ktest_begin("ksys: poll answers the state of the objects and waits in the scheduler");

    setup();

    KT_CHECK_INT_EQ(ksys_pipe(a), 0);
    KT_CHECK_INT_EQ(ksys_pipe(b), 0);
    KT_CHECK_INT_EQ(ksys_pipe(empty), 0);

    /* Two empty pipes: nothing is ready and a timeout of zero does not
     * wait. */
    memset(list, 0, sizeof(list));
    list[0].fd = (k_u32_t)a[0];
    list[0].events = KSYS_POLLIN;
    list[1].fd = (k_u32_t)b[0];
    list[1].events = KSYS_POLLIN;
    KT_CHECK_INT_EQ(ksys_poll(list, 2, 0), 0);
    KT_CHECK_INT_EQ(list[0].revents, 0);
    KT_CHECK_INT_EQ(list[1].revents, 0);

    /* A byte in the second pipe is the answer of the second entry only. */
    KT_CHECK_INT_EQ(ksys_write(b[1], "y", 1), 1);
    KT_CHECK_INT_EQ(ksys_poll(list, 2, 0), 1);
    KT_CHECK_INT_EQ(list[0].revents, 0);
    KT_CHECK_INT_EQ(list[1].revents, KSYS_POLLIN);
    KT_CHECK_INT_EQ(ksys_read(b[0], &byte, 1), 1);
    KT_CHECK_INT_EQ(byte, 'y');

    /* A wait that nothing ends is the deadline: the call answers zero and
     * the time it took is the time it was given. */
    list[0].fd = (k_u32_t)empty[0];
    list[0].events = KSYS_POLLIN;
    start = ksys_uptime();
    KT_CHECK_INT_EQ(ksys_poll(list, 1, 60), 0);
    elapsed = ksys_uptime() - start;
    KT_CHECK(elapsed >= 40);
    KT_CHECK(elapsed < 2000);

    /* A wait of no descriptor is a sleep: the scheduler keeps the processor
     * for the other tasks and the call answers when the time is over. */
    start = ksys_uptime();
    KT_CHECK_INT_EQ(ksys_poll(NULL, 0, 50), 0);
    elapsed = ksys_uptime() - start;
    KT_CHECK(elapsed >= 30);
    KT_CHECK(elapsed < 2000);

    /*
     * The wait that is ended by the object: a task of its own writes a byte
     * into the pipe after a moment, and the poll of the reader answers as
     * soon as the byte is there - not at its deadline, which is without one.
     */
    g_poll_helper.write_fd = a[1];
    g_poll_helper.signal_pid = 0;
    g_poll_helper.signal = 0;
    g_poll_helper.delay_ms = 30;

    helper = task_fork_shared(task_self(), "poll-writer", poll_helper_task, &g_poll_helper);
    KT_CHECK(helper != NULL);

    if (helper != NULL) {
        list[0].fd = (k_u32_t)a[0];
        list[0].events = KSYS_POLLIN;
        start = ksys_uptime();
        KT_CHECK_INT_EQ(ksys_poll(list, 1, -1), 1);
        elapsed = ksys_uptime() - start;
        KT_CHECK(list[0].revents == KSYS_POLLIN);
        KT_CHECK(elapsed < 400);
        KT_CHECK_INT_EQ(ksys_read(a[0], &byte, 1), 1);
        KT_CHECK_INT_EQ(task_wait(helper->pid, NULL, 2000), 0);
    }

    /* The end of the writing end is the end of the file: a read does not
     * wait, so the entry is ready. */
    KT_CHECK_INT_EQ(ksys_close(a[1]), 0);
    list[0].fd = (k_u32_t)a[0];
    list[0].events = KSYS_POLLIN;
    KT_CHECK_INT_EQ(ksys_poll(list, 1, 0), 1);
    KT_CHECK_INT_EQ(list[0].revents, KSYS_POLLIN);

    /* A writing end whose reader is gone is the error of the object. */
    KT_CHECK_INT_EQ(ksys_close(empty[0]), 0);
    list[0].fd = (k_u32_t)empty[1];
    list[0].events = KSYS_POLLOUT;
    KT_CHECK_INT_EQ(ksys_poll(list, 1, 0), 1);
    KT_CHECK_INT_EQ(list[0].revents, KSYS_POLLERR);

    /* A number that is not an open descriptor is the error of the entry, and
     * a negative number is ignored. */
    list[0].fd = 14;
    list[0].events = KSYS_POLLIN;
    list[1].fd = (k_u32_t)-1;
    list[1].events = KSYS_POLLIN;
    KT_CHECK_INT_EQ(ksys_poll(list, 2, 0), 1);
    KT_CHECK_INT_EQ(list[0].revents, KSYS_POLLERR);
    KT_CHECK_INT_EQ(list[1].revents, 0);

    /* The edge: a list that is too long, a list that is not there, and a
     * timeout that is not a timeout. */
    KT_CHECK_INT_EQ(ksys_poll(list, KSYS_POLL_MAX + 1, 0), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_poll(NULL, 1, 0), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_poll(list, 1, -2), KERR_INVAL);

    /*
     * A signal cuts the wait: the call answers EINTR and the handler of the
     * task runs. The signal is sent by a task of its own, because the task
     * that waits is the one that has to be interrupted.
     */
    g_poll_helper.write_fd = -1;
    g_poll_helper.signal_pid = (int)task_self()->pid;
    g_poll_helper.signal = K_SIGTERM;
    g_poll_helper.delay_ms = 30;

    helper = task_fork_shared(task_self(), "poll-signal", poll_helper_task, &g_poll_helper);
    KT_CHECK(helper != NULL);

    if (helper != NULL) {
        list[0].fd = (k_u32_t)b[0];
        list[0].events = KSYS_POLLIN;
        KT_CHECK(ksys_signal(K_SIGTERM, poll_ignore_signal) == K_SIG_DFL);
        start = ksys_uptime();
        KT_CHECK_INT_EQ(ksys_poll(list, 1, -1), KERR_INTR);
        elapsed = ksys_uptime() - start;
        KT_CHECK(elapsed < 400);
        (void)ksys_signal(K_SIGTERM, K_SIG_DFL);
        KT_CHECK_INT_EQ(task_wait(helper->pid, NULL, 2000), 0);
    }

    KT_CHECK_INT_EQ(ksys_close(a[0]), 0);
    KT_CHECK_INT_EQ(ksys_close(b[0]), 0);
    KT_CHECK_INT_EQ(ksys_close(b[1]), 0);
    KT_CHECK_INT_EQ(ksys_close(empty[1]), 0);

    teardown();
}

/*
 * The reader of a pipe whose writer is late. The read has nothing to take
 * and the writer is still there: the call waits, and the bytes arrive. An
 * answer of EAGAIN would be a machine that gave up on a pipeline that was
 * about to speak.
 */
static int g_pipe_slow_fds[2];
/* The flags of the cases below are the handshake between two tasks: one
 * writes, the other reads, and the scheduler of the host is two threads. An
 * atomic int is what that is in C11 - the race detector of the cloud named
 * the three of these that were a `volatile int`, and a volatile one is not a
 * synchronisation even where the read happens to be fresh. */
static _Atomic int g_pipe_slow_done;
static _Atomic int g_pipe_slow_result;
static char g_pipe_slow_text[8];

static void pipe_slow_program(void *arg)
{
    struct task *self = task_self();
    int pid;

    K_UNUSED(arg);

    if (!self->is_child) {
        g_pipe_slow_done = 0;
        g_pipe_slow_result = 0;
        memset(g_pipe_slow_text, 0, sizeof(g_pipe_slow_text));
        if (ksys_pipe(g_pipe_slow_fds) != 0) {
            g_pipe_slow_result = -100;
            g_pipe_slow_done = 1;
            return;
        }
    }

    pid = ksys_fork();
    if (pid == 0) {
        /* The writer: it is there, and it speaks after a moment. */
        k_sched_sleep_ms(20);
        (void)ksys_write(g_pipe_slow_fds[1], "hello", 5);
        ksys_close(g_pipe_slow_fds[1]);
        task_exit(0);
        return;
    }

    {
        int got = (int)ksys_read(g_pipe_slow_fds[0], g_pipe_slow_text, 5);

        g_pipe_slow_result = (got == 5) ? 1 : got;
    }

    ksys_close(g_pipe_slow_fds[0]);
    ksys_close(g_pipe_slow_fds[1]);
    if (pid > 0) {
        (void)ksys_waitpid(pid, NULL, 0);
    }

    g_pipe_slow_done = 1;
    task_exit(0);
}

static void test_pipe_waits(void)
{
    struct task *reader;
    int waited;

    ktest_begin("ksys: a read of an empty pipe waits for its writer and gets the bytes");

    setup();

    reader = task_create("piperead", pipe_slow_program, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(reader != NULL);

    for (waited = 0; waited < 3000 && !g_pipe_slow_done; waited++) {
        k_sched_sleep_ms(1);
    }

    KT_CHECK_INT_EQ(g_pipe_slow_done, 1);
    KT_CHECK_INT_EQ(g_pipe_slow_result, 1);
    KT_CHECK_STR_EQ(g_pipe_slow_text, "hello");

    if (reader != NULL) {
        KT_CHECK_INT_EQ(task_wait(reader->pid, NULL, 2000), 0);
    }

    teardown();
}

/*
 * The writer of a pipe whose reader is slow: the ring fills, the write
 * sleeps until the reader makes room, and it finishes the whole request.
 * The bound of half a second that the release put on a pipe is gone: a
 * reader slower than its writer is an ordinary pipeline and not an error.
 */
#define PIPE_GRAIN_TOTAL ((k_size_t)(2 * KFILE_PIPE_SIZE + 1024))

static int g_pipe_grain_fds[2];
static _Atomic int g_pipe_grain_done;
static _Atomic int g_pipe_grain_result;
static _Atomic int g_pipe_grain_early;

static void pipe_grain_program(void *arg)
{
    struct task *self = task_self();
    static char block[1024];
    static char sink[1024];
    k_size_t total = 0;
    int status = 0;
    int rc = 0;
    int pid;

    K_UNUSED(arg);

    if (!self->is_child) {
        g_pipe_grain_done = 0;
        g_pipe_grain_result = 0;
        g_pipe_grain_early = 0;
        memset(block, 'g', sizeof(block));
        memset(sink, 0, sizeof(sink));
        if (ksys_pipe(g_pipe_grain_fds) != 0) {
            g_pipe_grain_result = -100;
            g_pipe_grain_done = 1;
            return;
        }
    }

    pid = ksys_fork();
    if (pid == 0) {
        k_size_t done = 0;

        while (done < PIPE_GRAIN_TOTAL) {
            k_size_t want = K_MIN(sizeof(block), PIPE_GRAIN_TOTAL - done);

            rc = (int)ksys_write(g_pipe_grain_fds[1], block, want);
            if (rc <= 0) {
                break;
            }
            done += (k_size_t)rc;
        }

        ksys_close(g_pipe_grain_fds[1]);
        task_exit(rc < 0 ? 1 : 0);
        return;
    }

    /* The child fills the ring and stops there: it is alive because the
     * pipe is full, not because it is slow. */
    k_sched_sleep_ms(50);
    if (pid > 0 && ksys_waitpid(pid, &status, KSYS_WNOHANG) == pid) {
        g_pipe_grain_early = 1;
        g_pipe_grain_result = -3;
    }

    ksys_close(g_pipe_grain_fds[1]);

    for (;;) {
        rc = (int)ksys_read(g_pipe_grain_fds[0], sink, sizeof(sink));
        if (rc < 0) {
            g_pipe_grain_result = rc;
            break;
        }
        if (rc == 0) {
            break;
        }
        total += (k_size_t)rc;
    }

    ksys_close(g_pipe_grain_fds[0]);

    if (g_pipe_grain_result == 0) {
        g_pipe_grain_result = (total == PIPE_GRAIN_TOTAL) ? 1 : -2;
    }
    if (pid > 0 && !g_pipe_grain_early) {
        if (ksys_waitpid(pid, &status, 0) != pid) {
            g_pipe_grain_result = -4;
        } else if (status != 0) {
            g_pipe_grain_result = -5;
        }
    }

    g_pipe_grain_done = 1;
    task_exit(0);
}

static void test_pipe_slow_writer(void)
{
    struct task *writer;
    int waited;

    ktest_begin("ksys: a write to a full pipe sleeps until the reader makes room");

    setup();

    writer = task_create("pipegrain", pipe_grain_program, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(writer != NULL);

    for (waited = 0; waited < 5000 && !g_pipe_grain_done; waited++) {
        k_sched_sleep_ms(1);
    }

    KT_CHECK_INT_EQ(g_pipe_grain_done, 1);
    KT_CHECK_INT_EQ(g_pipe_grain_early, 0);
    KT_CHECK_INT_EQ(g_pipe_grain_result, 1);

    if (writer != NULL) {
        KT_CHECK_INT_EQ(task_wait(writer->pid, NULL, 2000), 0);
    }

    teardown();
}

/*
 * The promise of PIPE_BUF: a write of at most that many bytes lands in the
 * ring whole. The ring is full, a writer asks for one PIPE_BUF block, and
 * the reader makes room for half of it: the call does not take the half and
 * leave its caller to ask again - it waits for the whole block, and the
 * reader of the block gets one word and not two pieces of one.
 */
static int g_pipe_atomic_fds[2];
static _Atomic int g_pipe_atomic_done;
static _Atomic int g_pipe_atomic_result;
static _Atomic int g_pipe_atomic_blocked;
static char g_pipe_atomic_read[KFILE_PIPE_BUF];

static void pipe_atomic_program(void *arg)
{
    struct task *self = task_self();
    static char filler[KFILE_PIPE_SIZE];
    static char block[KFILE_PIPE_BUF];
    k_size_t left;
    k_size_t i;
    int status = 0;
    int pid;

    K_UNUSED(arg);

    if (!self->is_child) {
        g_pipe_atomic_done = 0;
        g_pipe_atomic_result = 0;
        g_pipe_atomic_blocked = 0;
        memset(filler, 'F', sizeof(filler));
        memset(block, 'A', sizeof(block));
        if (ksys_pipe(g_pipe_atomic_fds) != 0) {
            g_pipe_atomic_result = -100;
            g_pipe_atomic_done = 1;
            return;
        }

        /* The ring is full: not one byte of room. */
        if (ksys_write(g_pipe_atomic_fds[1], filler, sizeof(filler)) != (k_ssize_t)sizeof(filler)) {
            g_pipe_atomic_result = -10;
            g_pipe_atomic_done = 1;
            return;
        }
    }

    pid = ksys_fork();
    if (pid == 0) {
        int rc;

        k_sched_sleep_ms(10);
        rc = (int)ksys_write(g_pipe_atomic_fds[1], block, sizeof(block));
        ksys_close(g_pipe_atomic_fds[1]);
        task_exit(rc == (int)sizeof(block) ? 0 : 1);
        return;
    }

    k_sched_sleep_ms(30);

    /* Room for half of the block, and the writer is still waiting for the
     * whole of it. */
    if (ksys_read(g_pipe_atomic_fds[0], g_pipe_atomic_read, KFILE_PIPE_BUF / 2u) !=
        (k_ssize_t)(KFILE_PIPE_BUF / 2u)) {
        g_pipe_atomic_result = -11;
        goto out;
    }

    k_sched_sleep_ms(20);
    if (pid > 0 && ksys_waitpid(pid, &status, KSYS_WNOHANG) == pid) {
        g_pipe_atomic_blocked = 1;
        g_pipe_atomic_result = -12;
        goto out;
    }

    /* The rest of the filler: the block of the writer follows it and is the
     * only thing left in the ring. */
    left = KFILE_PIPE_SIZE - (k_size_t)KFILE_PIPE_BUF / 2u;
    while (left > 0) {
        k_size_t want = K_MIN(left, sizeof(g_pipe_atomic_read));

        if (ksys_read(g_pipe_atomic_fds[0], g_pipe_atomic_read, want) != (k_ssize_t)want) {
            g_pipe_atomic_result = -13;
            goto out;
        }
        left -= want;
    }

    if (ksys_read(g_pipe_atomic_fds[0], g_pipe_atomic_read, sizeof(g_pipe_atomic_read)) !=
        (k_ssize_t)sizeof(g_pipe_atomic_read)) {
        g_pipe_atomic_result = -14;
        goto out;
    }

    for (i = 0; i < KFILE_PIPE_BUF; i++) {
        if ((unsigned char)g_pipe_atomic_read[i] != (unsigned char)'A') {
            g_pipe_atomic_result = -15;
            goto out;
        }
    }

    g_pipe_atomic_result = 1;

out:
    ksys_close(g_pipe_atomic_fds[0]);
    ksys_close(g_pipe_atomic_fds[1]);

    if (pid > 0 && g_pipe_atomic_result != -12) {
        if (ksys_waitpid(pid, &status, 0) != pid || status != 0) {
            g_pipe_atomic_result = -16;
        }
    }

    g_pipe_atomic_done = 1;
    task_exit(0);
}

static void test_pipe_atomic(void)
{
    struct task *program;
    int waited;

    ktest_begin("ksys: a write of PIPE_BUF waits for the whole block and lands whole");

    setup();

    program = task_create("pipeatom", pipe_atomic_program, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(program != NULL);

    for (waited = 0; waited < 5000 && !g_pipe_atomic_done; waited++) {
        k_sched_sleep_ms(1);
    }

    KT_CHECK_INT_EQ(g_pipe_atomic_done, 1);
    KT_CHECK_INT_EQ(g_pipe_atomic_blocked, 0);
    KT_CHECK_INT_EQ(g_pipe_atomic_result, 1);

    if (program != NULL) {
        KT_CHECK_INT_EQ(task_wait(program->pid, NULL, 2000), 0);
    }

    teardown();
}

/*
 * The property: dup2 gives a second descriptor of one open file, the write
 * goes to the object the number now names, and closing the original leaves
 * the copy alive.
 */
static void test_dup2(void)
{
    char out[16];
    int fds[2];
    int file;
    int copy;

    ktest_begin("ksys: dup2 makes a second descriptor of one open file");

    setup();
    memset(out, 0, sizeof(out));

    file = ksys_open("/dup.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(file >= 0);

    copy = ksys_dup2(file, 1);
    KT_CHECK_INT_EQ(copy, 1);

    KT_CHECK_INT_EQ(ksys_write(1, "abc", 3), 3);
    KT_CHECK_INT_EQ(ksys_close(file), 0); /* the copy keeps the file open */
    KT_CHECK_INT_EQ(ksys_write(1, "def", 3), 3);
    KT_CHECK_INT_EQ(ksys_close(1), 0);

    file = ksys_open("/dup.txt", KSYS_O_RDONLY);
    KT_CHECK(file >= 0);
    KT_CHECK_INT_EQ(ksys_read(file, out, sizeof(out)), 6);
    KT_CHECK_STR_EQ(out, "abcdef");
    KT_CHECK_INT_EQ(ksys_close(file), 0);

    /* The descriptor that was open at the new number is closed by the call:
     * the pipe loses its writing end and the reader gets the end of the
     * file without waiting. */
    KT_CHECK_INT_EQ(ksys_pipe(fds), 0);
    file = ksys_open("/dup2.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(file >= 0);
    KT_CHECK_INT_EQ(ksys_dup2(file, fds[1]), fds[1]);
    KT_CHECK_INT_EQ(ksys_read(fds[0], out, sizeof(out)), 0);
    KT_CHECK_INT_EQ(ksys_close(fds[0]), 0);
    KT_CHECK_INT_EQ(ksys_write(fds[1], "pipe", 4), 4);
    KT_CHECK_INT_EQ(ksys_close(file), 0);
    KT_CHECK_INT_EQ(ksys_close(fds[1]), 0);

    /* dup2 sends the writes of the number 1 into a pipe: the ends take the
     * first free slots, so two files hold 0 and 1 and the new number 1
     * closes the second of them. */
    KT_CHECK_INT_EQ(ksys_open("/dup4.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC), 0);
    KT_CHECK_INT_EQ(ksys_open("/dup5.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC), 1);
    KT_CHECK_INT_EQ(ksys_pipe(fds), 0);
    KT_CHECK_INT_EQ(ksys_dup2(fds[1], 1), 1);
    KT_CHECK_INT_EQ(ksys_close(fds[1]), 0);
    KT_CHECK_INT_EQ(ksys_write(1, "pipe", 4), 4);
    KT_CHECK_INT_EQ(ksys_close(1), 0);
    KT_CHECK_INT_EQ(ksys_close(0), 0);
    memset(out, 0, sizeof(out));
    KT_CHECK_INT_EQ(ksys_read(fds[0], out, sizeof(out)), 4);
    KT_CHECK_STR_EQ(out, "pipe");
    KT_CHECK_INT_EQ(ksys_close(fds[0]), 0);

    teardown();
}

/*
 * The property: a number that is not a descriptor of the task is refused,
 * and the two calls leave the table as it was.
 */
static void test_pipe_errors(void)
{
    int file;

    ktest_begin("ksys: a descriptor a pipe or a dup2 cannot use is refused");

    setup();

    KT_CHECK_INT_EQ(ksys_pipe(NULL), KERR_INVAL);

    /* The table is empty: there is nothing to copy. */
    KT_CHECK_INT_EQ(ksys_dup2(0, 1), KERR_BADF);
    KT_CHECK_INT_EQ(ksys_dup2(-1, 0), KERR_BADF);
    KT_CHECK_INT_EQ(ksys_dup2(0, TASK_MAX_FDS), KERR_BADF);
    KT_CHECK_INT_EQ(ksys_dup2(TASK_MAX_FDS, 0), KERR_BADF);

    file = ksys_open("/dup3.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(file >= 0);
    if (file >= 0) {
        /* The same number twice changes nothing, and a number past the
         * table is not a descriptor. */
        KT_CHECK_INT_EQ(ksys_dup2(file, file), file);
        KT_CHECK_INT_EQ(ksys_dup2(file, TASK_MAX_FDS), KERR_BADF);
        KT_CHECK_INT_EQ(ksys_close(file), 0);
    }
    KT_CHECK_INT_EQ(ksys_close(file), KERR_BADF);

    teardown();
}

/* The answer of a task that waits without having a child of its own. */
static int g_no_child_any = 1;
static int g_no_child_blocking = 1;

static void wait_without_a_child(void *arg)
{
    int status = -1;

    K_UNUSED(arg);

    g_no_child_any = ksys_waitpid(-1, &status, KSYS_WNOHANG);
    g_no_child_blocking = ksys_waitpid(-1, &status, 0);
}

/*
 * The property: a wait without a child is ECHILD. There is nothing to wait
 * for, whether the caller was ready to wait or not. The task of the test may
 * have children of its own by now, so the wait is made in a task that has
 * none.
 */
static void test_wait_no_child(void)
{
    struct task *waiter;
    int status = -1;

    ktest_begin("ksys: a wait without a child is ECHILD");

    setup();

    /* A number that names no child of this task is one more way. */
    KT_CHECK_INT_EQ(ksys_waitpid(4242, &status, KSYS_WNOHANG), KERR_CHILD);
    KT_CHECK_INT_EQ(ksys_waitpid(4242, &status, 0), KERR_CHILD);

    waiter = task_create("waiter", wait_without_a_child, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(waiter != NULL);
    if (waiter != NULL) {
        KT_CHECK_INT_EQ(task_wait(waiter->pid, NULL, 2000), 0);
        KT_CHECK_INT_EQ(g_no_child_any, KERR_CHILD);
        KT_CHECK_INT_EQ(g_no_child_blocking, KERR_CHILD);
    }

    teardown();
}

/*
 * The property: a fresh task starts at the ceiling of every resource, a
 * limit may be lowered, and a raise is refused. The test runs in a task of
 * its own because a limit belongs to the task and the runner is one task for
 * every test: a limit lowered here would follow the next test.
 */
static int g_limit_rc[8];
static k_u64_t g_limit_value[4];

static void limits_defaults_task(void *arg)
{
    struct task *self = task_self();

    K_UNUSED(arg);

    g_limit_rc[0] = ksys_getlimit(KSYS_RLIMIT_MEMORY, &g_limit_value[0]);
    g_limit_rc[1] = ksys_getlimit(KSYS_RLIMIT_CPU, &g_limit_value[1]);
    g_limit_rc[2] = ksys_getlimit(KSYS_RLIMIT_FDS, &g_limit_value[2]);
    g_limit_value[3] = (self != NULL) ? self->mem_used : 1u;

    /* A selector outside the three and a place that is not there. */
    g_limit_rc[3] = ksys_getlimit(0, &g_limit_value[0]);
    g_limit_rc[4] = ksys_getlimit(KSYS_RLIMIT_FDS + 1, &g_limit_value[0]);
    g_limit_rc[5] = ksys_getlimit(KSYS_RLIMIT_FDS, NULL);
    g_limit_rc[6] = ksys_setlimit(0, 0);
    g_limit_rc[7] = ksys_setlimit(KSYS_RLIMIT_MEMORY, (k_u64_t)KSYS_RLIMIT_MEMORY_MAX + 1u);
}

static void test_limits_defaults(void)
{
    struct task *worker;

    ktest_begin("ksys: a task starts at the ceiling of its resources");

    setup();

    worker = task_create("limits-defaults", limits_defaults_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(worker != NULL);

    if (worker != NULL) {
        KT_CHECK_INT_EQ(task_wait(worker->pid, NULL, 2000), 0);

        KT_CHECK_INT_EQ(g_limit_rc[0], 0);
        KT_CHECK_INT_EQ(g_limit_rc[1], 0);
        KT_CHECK_INT_EQ(g_limit_rc[2], 0);
        KT_CHECK_INT_EQ((int)g_limit_value[0], (int)KSYS_RLIMIT_MEMORY_MAX);
        KT_CHECK_INT_EQ((int)g_limit_value[1], (int)KSYS_RLIMIT_CPU_MAX);
        KT_CHECK_INT_EQ((int)g_limit_value[2], (int)KSYS_RLIMIT_FDS_MAX);
        KT_CHECK_INT_EQ((int)g_limit_value[3], 0);

        KT_CHECK_INT_EQ(g_limit_rc[3], KERR_INVAL);
        KT_CHECK_INT_EQ(g_limit_rc[4], KERR_INVAL);
        KT_CHECK_INT_EQ(g_limit_rc[5], KERR_INVAL);
        KT_CHECK_INT_EQ(g_limit_rc[6], KERR_INVAL);
        KT_CHECK_INT_EQ(g_limit_rc[7], KERR_PERM);
    }

    teardown();
}

/*
 * The property: the descriptor limit decides how many files a task may
 * open, and a number at or above it is EMFILE. Lowering the limit does not
 * close what is already open, and no call can raise the limit back.
 */
static int g_fd_limit_rc[12];

static void limits_descriptors_task(void *arg)
{
    int fds[2];

    K_UNUSED(arg);

    g_fd_limit_rc[0] = ksys_open("/l0.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    g_fd_limit_rc[1] = ksys_open("/l1.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    g_fd_limit_rc[2] = ksys_open("/l2.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);

    /* The limit of the table is four: all three opened. */
    g_fd_limit_rc[3] = ksys_setlimit(KSYS_RLIMIT_FDS, 2);
    /* The descriptor above the limit stays open and usable. */
    g_fd_limit_rc[4] = (g_fd_limit_rc[2] >= 0) ? ksys_write(g_fd_limit_rc[2], "x", 1) : -1;
    g_fd_limit_rc[5] = (g_fd_limit_rc[2] >= 0) ? ksys_dup2(g_fd_limit_rc[2], g_fd_limit_rc[2]) : -1;
    /* A new descriptor, a new number and a pipe all stop at the limit. */
    g_fd_limit_rc[6] = ksys_open("/l3.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);
    g_fd_limit_rc[7] = (g_fd_limit_rc[0] >= 0) ? ksys_dup2(g_fd_limit_rc[0], 2) : -1;
    g_fd_limit_rc[8] = ksys_pipe(fds);
    /* The one free slot below the limit cannot hold a pipe. */
    g_fd_limit_rc[9] = (g_fd_limit_rc[1] >= 0) ? ksys_close(g_fd_limit_rc[1]) : -1;
    g_fd_limit_rc[10] = ksys_pipe(fds);
    /* And the limit does not go back up. */
    g_fd_limit_rc[11] = ksys_setlimit(KSYS_RLIMIT_FDS, TASK_MAX_FDS);

    if (g_fd_limit_rc[0] >= 0) {
        ksys_close(g_fd_limit_rc[0]);
    }
    if (g_fd_limit_rc[2] >= 0) {
        ksys_close(g_fd_limit_rc[2]);
    }
}

static void test_limits_descriptors(void)
{
    struct task *worker;

    ktest_begin("ksys: the descriptor limit stops open, dup2 and pipe at EMFILE");

    setup();

    worker = task_create("limits-fds", limits_descriptors_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(worker != NULL);

    if (worker != NULL) {
        KT_CHECK_INT_EQ(task_wait(worker->pid, NULL, 2000), 0);

        KT_CHECK(g_fd_limit_rc[0] >= 0);
        KT_CHECK(g_fd_limit_rc[1] >= 0);
        KT_CHECK(g_fd_limit_rc[2] >= 0);
        KT_CHECK_INT_EQ(g_fd_limit_rc[3], 0);
        KT_CHECK_INT_EQ(g_fd_limit_rc[4], 1);
        KT_CHECK_INT_EQ(g_fd_limit_rc[5], g_fd_limit_rc[2]);
        KT_CHECK_INT_EQ(g_fd_limit_rc[6], KERR_MFILE);
        KT_CHECK_INT_EQ(g_fd_limit_rc[7], KERR_MFILE);
        KT_CHECK_INT_EQ(g_fd_limit_rc[8], KERR_MFILE);
        KT_CHECK_INT_EQ(g_fd_limit_rc[9], 0);
        KT_CHECK_INT_EQ(g_fd_limit_rc[10], KERR_MFILE);
        KT_CHECK_INT_EQ(g_fd_limit_rc[11], KERR_PERM);
    }

    teardown();
}

/*
 * The property: the memory of the user blocks of a task is counted, a block
 * over the limit is ENOMEM, and giving a block back returns the room to the
 * task. The kernel allocations are not charged: the limit is of the task and
 * not of the system.
 */
static k_ssize_t g_mem_limit_rc[6];
static k_u64_t g_mem_used[3];

static void limits_memory_task(void *arg)
{
    struct task *self = task_self();
    void *block;

    K_UNUSED(arg);

    g_mem_limit_rc[0] = ksys_setlimit(KSYS_RLIMIT_MEMORY, 4096);

    block = ksys_malloc(2048);
    g_mem_limit_rc[1] = (block != NULL) ? 0 : KERR_NOMEM;
    g_mem_used[0] = (self != NULL) ? self->mem_used : 1u;

    /* 2048 + 4096 is over the budget: the second block is refused. */
    g_mem_limit_rc[2] = (ksys_malloc(4096) == NULL) ? KERR_NOMEM : 0;

    ksys_free(block);
    g_mem_used[1] = (self != NULL) ? self->mem_used : 1u;

    /* The room came back: the same 4096 now fits. */
    block = ksys_malloc(4096);
    g_mem_limit_rc[3] = (block != NULL) ? 0 : KERR_NOMEM;
    ksys_free(block);
    g_mem_used[2] = (self != NULL) ? self->mem_used : 1u;

    /* A budget of zero refuses any block at all. */
    g_mem_limit_rc[4] = ksys_setlimit(KSYS_RLIMIT_MEMORY, 0);
    g_mem_limit_rc[5] = (ksys_malloc(1) == NULL) ? KERR_NOMEM : 0;
}

static void test_limits_memory(void)
{
    struct task *worker;

    ktest_begin("ksys: the memory limit refuses a block and gives the room back");

    setup();

    worker = task_create("limits-memory", limits_memory_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(worker != NULL);

    if (worker != NULL) {
        KT_CHECK_INT_EQ(task_wait(worker->pid, NULL, 2000), 0);

        KT_CHECK_INT_EQ(g_mem_limit_rc[0], 0);
        KT_CHECK_INT_EQ(g_mem_limit_rc[1], 0);
        KT_CHECK(g_mem_used[0] >= 2048u);
        KT_CHECK_INT_EQ(g_mem_limit_rc[2], KERR_NOMEM);
        KT_CHECK_INT_EQ((int)g_mem_used[1], 0);
        KT_CHECK_INT_EQ(g_mem_limit_rc[3], 0);
        KT_CHECK_INT_EQ((int)g_mem_used[2], 0);
        KT_CHECK_INT_EQ(g_mem_limit_rc[4], 0);
        KT_CHECK_INT_EQ(g_mem_limit_rc[5], KERR_NOMEM);
    }

    teardown();
}

/*
 * The property: the CPU time of a task is counted in ticks of a millisecond,
 * and a task that reaches its limit is marked with SIGTERM (the closest
 * signal of the four to SIGXCPU): the default action ends it with 128 + 15.
 */
static int g_cpu_limit_rc;
static _Atomic int g_cpu_limit_done;

static void limits_cpu_task(void *arg)
{
    k_u64_t start = k_sched_uptime_ms();

    K_UNUSED(arg);

    g_cpu_limit_rc = ksys_setlimit(KSYS_RLIMIT_CPU, 20);

    /*
     * Every yield is a system call, and the way out of a call is the point
     * where the mark is answered. The loop is bounded so that a machine on
     * which the mark never arrives still ends the test: a task that never
     * exits would keep the next test from running.
     */
    while (k_sched_uptime_ms() - start < 3000u) {
        ksys_yield();
    }

    g_cpu_limit_done = 1;
}

static void test_limits_cpu(void)
{
    struct task *worker;
    int status = 0;

    ktest_begin("ksys: the CPU limit marks the task with SIGTERM");

    setup();

    g_cpu_limit_rc = KERR_INVAL;
    g_cpu_limit_done = 0;

    worker = task_create("limits-cpu", limits_cpu_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(worker != NULL);

    if (worker != NULL) {
        KT_CHECK_INT_EQ(task_wait(worker->pid, &status, 5000), 0);
        KT_CHECK_INT_EQ(g_cpu_limit_rc, 0);
        /* The default action of SIGTERM is the end of the task. */
        KT_CHECK_INT_EQ(status, 128 + K_SIGTERM);
        KT_CHECK_INT_EQ(g_cpu_limit_done, 0);
    }

    teardown();
}

/*
 * The property: a task holds sixteen descriptors at once, the seventeenth is
 * EMFILE, and a dup2 may name the top of the table. The limit is the table
 * of the task and not the table of the descriptions: sixteen files of one
 * task fit beside the ends of the pipes of the shell.
 */
static void test_fd_table_full(void)
{
    int fds[TASK_MAX_FDS];
    int i;

    ktest_begin("ksys: sixteen descriptors are open and the seventeenth is EMFILE");

    setup();

    for (i = 0; i < TASK_MAX_FDS; i++) {
        fds[i] = ksys_open("/many-fds.txt", KSYS_O_RDWR | KSYS_O_CREAT);
        KT_CHECK_INT_EQ(fds[i], i);
    }

    KT_CHECK_INT_EQ(ksys_open("/many-fds.txt", KSYS_O_RDONLY), KERR_MFILE);

    /* The last number of the table is a descriptor like any other. */
    KT_CHECK_INT_EQ(ksys_close(fds[TASK_MAX_FDS - 1]), 0);
    fds[TASK_MAX_FDS - 1] = ksys_dup2(fds[0], TASK_MAX_FDS - 1);
    KT_CHECK_INT_EQ(fds[TASK_MAX_FDS - 1], TASK_MAX_FDS - 1);
    KT_CHECK_INT_EQ(ksys_write(fds[TASK_MAX_FDS - 1], "x", 1), 1);

    /* The number one past the table is not one, whatever is free. */
    KT_CHECK_INT_EQ(ksys_dup2(fds[0], TASK_MAX_FDS), KERR_BADF);

    for (i = 0; i < TASK_MAX_FDS; i++) {
        if (fds[i] >= 0) {
            KT_CHECK_INT_EQ(ksys_close(fds[i]), 0);
        }
    }

    teardown();
}

/*
 * The property: ftruncate cuts an open file, the position of the descriptor
 * does not move, a growth reads as zeros, and a reader, a pipe and a number
 * that is not open have no cut.
 */
static void test_ftruncate(void)
{
    static char content[4096];
    static char buffer[4096];
    struct vfs_stat info;
    int fds[2];
    int file;
    int i;

    ktest_begin("ksys: ftruncate cuts and grows an open file");

    setup();

    for (i = 0; i < (int)sizeof(content); i++) {
        content[i] = (char)('A' + (i % 26));
    }

    file = ksys_open("/cut.bin", KSYS_O_RDWR | KSYS_O_CREAT | KSYS_O_TRUNC);
    KT_CHECK(file >= 0);
    if (file < 0) {
        teardown();
        return;
    }

    KT_CHECK_INT_EQ(ksys_write(file, content, sizeof(content)), (int)sizeof(content));

    KT_CHECK_INT_EQ(ksys_ftruncate(file, 1000), 0);
    KT_CHECK_INT_EQ(ksys_stat("/cut.bin", &info), 0);
    KT_CHECK_INT_EQ(info.size, 1000);

    KT_CHECK_INT_EQ(ksys_lseek(file, 0, KSYS_SEEK_SET), 0);
    memset(buffer, 0, sizeof(buffer));
    KT_CHECK_INT_EQ(ksys_read(file, buffer, sizeof(buffer)), 1000);
    KT_CHECK(memcmp(buffer, content, 1000) == 0);
    KT_CHECK_INT_EQ(ksys_read(file, buffer, sizeof(buffer)), 0);

    /* The growth is a hole of zeros, and the position of the descriptor is
     * where the last read left it. */
    KT_CHECK_INT_EQ(ksys_ftruncate(file, 2000), 0);
    KT_CHECK_INT_EQ(ksys_lseek(file, 1000, KSYS_SEEK_SET), 1000);
    memset(buffer, 0xFF, sizeof(buffer));
    KT_CHECK_INT_EQ(ksys_read(file, buffer, sizeof(buffer)), 1000);
    for (i = 0; i < 1000; i++) {
        KT_CHECK_INT_EQ(buffer[i], 0);
    }
    KT_CHECK_INT_EQ(ksys_lseek(file, 0, KSYS_SEEK_CUR), 2000);

    /* A size wider than the record of the filesystem is refused before a
     * byte is cut. */
    KT_CHECK_INT_EQ(ksys_ftruncate(file, (k_u64_t)0x100000000ull), KERR_TOOBIG);
    KT_CHECK_INT_EQ(ksys_stat("/cut.bin", &info), 0);
    KT_CHECK_INT_EQ(info.size, 2000);

    /* A cut does not move the position, even when it is past the new size. */
    KT_CHECK_INT_EQ(ksys_ftruncate(file, 500), 0);
    KT_CHECK_INT_EQ(ksys_lseek(file, 0, KSYS_SEEK_CUR), 2000);
    KT_CHECK_INT_EQ(ksys_close(file), 0);

    /* A reader, a pipe and a number that names nothing. */
    file = ksys_open("/cut.bin", KSYS_O_RDONLY);
    KT_CHECK(file >= 0);
    if (file >= 0) {
        KT_CHECK_INT_EQ(ksys_ftruncate(file, 0), KERR_BADF);
        KT_CHECK_INT_EQ(ksys_close(file), 0);
    }

    KT_CHECK_INT_EQ(ksys_pipe(fds), 0);
    KT_CHECK_INT_EQ(ksys_ftruncate(fds[1], 0), KERR_INVAL);
    KT_CHECK_INT_EQ(ksys_close(fds[0]), 0);
    KT_CHECK_INT_EQ(ksys_close(fds[1]), 0);

    KT_CHECK_INT_EQ(ksys_ftruncate(TASK_MAX_FDS - 1, 0), KERR_BADF);

    teardown();
}

/* A child of a fork sees the descriptors of the parent and can close them. */
static _Atomic int g_inherited_fds;

static void fd_inheriting_child(void *arg)
{
    struct task *self = task_self();
    int i;

    K_UNUSED(arg);

    g_inherited_fds = 0;

    if (self != NULL) {
        for (i = 0; i < TASK_MAX_FDS; i++) {
            if (self->files[i] != NULL) {
                g_inherited_fds++;
                ksys_close(i);
            }
        }
    }
}

/*
 * The property: fstat answers the object of an open descriptor with the same
 * numbers statx gives for its name, and an exclusive create refuses a name
 * that is already there without touching it.
 */
static void test_fstat_and_exclusive(void)
{
    struct vfs_statx info;
    int fd;

    ktest_begin("ksys: fstat describes an open object and O_EXCL refuses a name");

    setup();

    fd = ksys_open("/fstat.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC | KSYS_O_EXCL);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, "fstat", 5), 5);
        KT_CHECK_INT_EQ(ksys_fstat(fd, &info), 0);
        KT_CHECK_INT_EQ(info.type, 1);
        KT_CHECK_INT_EQ(info.size, 5);
        KT_CHECK_INT_EQ(info.mode, 0644);
        KT_CHECK_INT_EQ(info.owner, 0);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* The name is there: the same exclusive create is an error and leaves
     * the bytes of the object alone, and a plain open finds them. */
    KT_CHECK_INT_EQ(
        ksys_open("/fstat.txt", KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC | KSYS_O_EXCL),
        KERR_EXIST);
    fd = ksys_open("/fstat.txt", KSYS_O_RDONLY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_fstat(fd, &info), 0);
        KT_CHECK_INT_EQ(info.size, 5);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }

    /* A directory is described as a directory; a pipe has no name and no
     * fstat (the library describes it from ESPIPE). */
    KT_CHECK_INT_EQ(ksys_mkdir("/fstat-dir"), 0);
    fd = ksys_open("/fstat-dir", KSYS_O_RDONLY | KSYS_O_DIRECTORY);
    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_fstat(fd, &info), 0);
        KT_CHECK_INT_EQ(info.type, 2);
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
    KT_CHECK_INT_EQ(ksys_rmdir("/fstat-dir"), 0);

    {
        int fds[2];

        KT_CHECK_INT_EQ(ksys_pipe(fds), 0);
        KT_CHECK_INT_EQ(ksys_fstat(fds[0], &info), KERR_SPIPE);
        KT_CHECK_INT_EQ(ksys_close(fds[0]), 0);
        KT_CHECK_INT_EQ(ksys_close(fds[1]), 0);
    }

    KT_CHECK_INT_EQ(ksys_unlink("/fstat.txt"), 0);

    teardown();
}

/*
 * The property: a fork copies the whole table of the descriptors, all
 * sixteen of them, and the child closes its own copy without touching the
 * parent's. The program of an execve inherits the same table through the
 * port; the x86-64 smoke test is where that half is read.
 */
static void test_fd_inherit_fork(void)
{
    struct task *child;
    int fds[TASK_MAX_FDS];
    int i;

    ktest_begin("ksys: a fork inherits all sixteen descriptors");

    setup();

    for (i = 0; i < TASK_MAX_FDS; i++) {
        fds[i] = ksys_open("/inherit-fds.txt", KSYS_O_RDWR | KSYS_O_CREAT);
        KT_CHECK_INT_EQ(fds[i], i);
    }

    g_inherited_fds = -1;
    child = task_fork_shared(task_self(), "fd-child", fd_inheriting_child, NULL);
    KT_CHECK(child != NULL);

    if (child != NULL) {
        KT_CHECK_INT_EQ(task_wait(child->pid, NULL, 2000), 0);
        KT_CHECK_INT_EQ(g_inherited_fds, TASK_MAX_FDS);
    }

    /* The child closed its references; the descriptors of the parent are
     * still open and still usable. */
    for (i = 0; i < TASK_MAX_FDS; i++) {
        if (fds[i] >= 0) {
            KT_CHECK_INT_EQ(ksys_write(fds[i], "x", 1), 1);
            KT_CHECK_INT_EQ(ksys_close(fds[i]), 0);
        }
    }

    teardown();
}

void test_ksys(void)
{
    test_abi_is_stable();
    test_abi_golden();
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
    test_statx();
    test_open_mode();
    test_directory_search();
    test_readdir_validation();
    test_readdir_directory();
    test_pipe();
    test_pipe_block();
    test_pipe_waits();
    test_pipe_slow_writer();
    test_pipe_atomic();
    test_dup2();
    test_pipe_errors();
    test_poll();
    test_mmap_edges();
    test_fd_table_full();
    test_ftruncate();
    test_fstat_and_exclusive();
    test_fd_inherit_fork();
    test_wait_no_child();
    test_limits_defaults();
    test_limits_descriptors();
    test_limits_memory();
    test_limits_cpu();
}
