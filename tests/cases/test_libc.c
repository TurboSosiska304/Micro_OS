/*
 * Micro OS — the tests of the C library.
 *
 * The library is checked against the library of the system: the same real
 * data (the corpus of the project) has to give the same result, no matter
 * which of the two libraries formats it.
 */
#include "ktest.h"
#include "tests/tests.h"

/*
 * The library of the system comes first: its functions are needed under
 * their own names, to compare the results. The library of Micro OS comes
 * after and takes the usual names for itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int (*sys_snprintf)(char *, size_t, const char *, ...) = snprintf;

#include "libc/stdio.h"
#include "libc/unistd.h"

#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

#define MAX_LINES 512
#define LINE_MAX 64

static struct mfs_ram_device g_volume;
static struct mfs_device g_device;

static char g_lines[MAX_LINES][LINE_MAX];
static int g_count;

static char g_buffer[16384];
static char g_reference[16384];

static void setup(void)
{
    if (mfs_ram_init(&g_volume, 512) != 0) {
        return;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 64);
    mfs_mount(&g_device);
    mfs_vfs_register("/");
    ksys_init();

    g_count = 0;
}

static void teardown(void)
{
    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

/* The two numbers are the same when they differ less than this. */
static int nearly(double a, double b)
{
    double diff = a - b;
    double scale = (a < 0.0) ? -a : a;

    if (diff < 0.0) {
        diff = -diff;
    }
    if (scale < 1.0) {
        scale = 1.0;
    }

    return diff <= scale * 1e-9;
}

/* Read the corpus: the real numbers of the project. */
static void load_corpus(void)
{
    FILE *file = fopen(KTEST_CORPUS, "r");
    char line[LINE_MAX];

    g_count = 0;

    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL && g_count < MAX_LINES) {
        size_t len = strlen(line);

        if (len > 0 && line[len - 1] == '\n') {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue;
        }

        memcpy(g_lines[g_count], line, len + 1);
        g_count++;
    }

    fclose(file);
}

/*
 * The property: the number that the library of Micro OS prints and the
 * number that the library of the system prints are the same number.
 */
static void test_float_matches_the_system(void)
{
    int i;
    int checked = 0;

    ktest_begin("libc: the float that the library prints is the number of the corpus");

    setup();
    load_corpus();
    KT_CHECK(g_count > 0);

    for (i = 0; i < g_count; i++) {
        double value = strtod(g_lines[i], NULL);
        double ours;
        double theirs;
        int n1;
        int n2;

        /* %f with the default precision. */
        n1 = microos_snprintf(g_buffer, sizeof(g_buffer), "%f", value);
        n2 = sys_snprintf(g_reference, sizeof(g_reference), "%f", value);
        KT_CHECK_INT_EQ(n1, n2);
        ours = strtod(g_buffer, NULL);
        theirs = strtod(g_reference, NULL);
        KT_CHECK(nearly(ours, theirs));

        /* %e and %g. */
        n1 = microos_snprintf(g_buffer, sizeof(g_buffer), "%e", value);
        n2 = sys_snprintf(g_reference, sizeof(g_reference), "%e", value);
        KT_CHECK_INT_EQ(n1, n2);
        ours = strtod(g_buffer, NULL);
        theirs = strtod(g_reference, NULL);
        KT_CHECK(nearly(ours, theirs));

        n1 = microos_snprintf(g_buffer, sizeof(g_buffer), "%.12g", value);
        n2 = sys_snprintf(g_reference, sizeof(g_reference), "%.12g", value);
        KT_CHECK_INT_EQ(n1, n2);
        ours = strtod(g_buffer, NULL);
        theirs = strtod(g_reference, NULL);
        KT_CHECK(nearly(ours, theirs));

        /* The precision changes the form, not the number. */
        n1 = microos_snprintf(g_buffer, sizeof(g_buffer), "%.3f", value);
        n2 = sys_snprintf(g_reference, sizeof(g_reference), "%.3f", value);
        KT_CHECK_INT_EQ(n1, n2);
        ours = strtod(g_buffer, NULL);
        theirs = strtod(g_reference, NULL);
        KT_CHECK(nearly(ours, theirs) || nearly(ours * 1000.0, theirs * 1000.0));

        checked++;
    }

    KT_CHECK_INT_EQ(checked, g_count);

    teardown();
}

/*
 * The property: the whole numbers of the corpus are printed the same way
 * by both libraries, with every flag.
 */
static void test_integer_matches_the_system(void)
{
    static const char *formats[] = {"%d",  "%u",  "%x",   "%X",     "%08d",  "%-10d",
                                    "%+d", "% d", "%10d", "%-10d",  "%#x",   "%20.5d",
                                    "%ld", "%lu", "%llx", "%08llu", "%-20u", "%+020d"};
    const int nformats = (int)(sizeof(formats) / sizeof(formats[0]));
    int i;
    int f;

    ktest_begin("libc: the whole numbers of the corpus are printed the same way");

    setup();
    load_corpus();
    KT_CHECK(g_count > 0);

    for (i = 0; i < g_count; i++) {
        long long value;
        char *end = NULL;

        /* Only the lines that are whole numbers. */
        if (strpbrk(g_lines[i], ".eE") != NULL) {
            continue;
        }

        value = strtoll(g_lines[i], &end, 10);
        if (end == g_lines[i] || (end != NULL && *end != '\0')) {
            continue;
        }

        for (f = 0; f < nformats; f++) {
            int n1 = microos_snprintf(g_buffer, sizeof(g_buffer), formats[f], value);
            int n2 = sys_snprintf(g_reference, sizeof(g_reference), formats[f], value);

            KT_CHECK_INT_EQ(n1, n2);
            if (strcmp(g_buffer, g_reference) != 0) {
                KT_CHECK_STR_EQ(g_buffer, g_reference);
                break;
            }
        }
    }

    teardown();
}

/*
 * The property: the strings are printed the same way, with the width and
 * the precision.
 */
static void test_string_matches_the_system(void)
{
    static const char *formats[] = {"%s", "%10s", "%-10s", "%.5s", "%20.10s", "[%s]"};
    const int nformats = (int)(sizeof(formats) / sizeof(formats[0]));
    int i;
    int f;

    ktest_begin("libc: the strings of the corpus are printed the same way");

    setup();
    load_corpus();
    KT_CHECK(g_count > 0);

    for (i = 0; i < g_count; i += 7) {
        char text[LINE_MAX + 16];

        /* A real string, built from the real numbers. */
        snprintf(text, sizeof(text), "value-%s.bin", g_lines[i]);

        for (f = 0; f < nformats; f++) {
            int n1 = microos_snprintf(g_buffer, sizeof(g_buffer), formats[f], text);
            int n2 = sys_snprintf(g_reference, sizeof(g_reference), formats[f], text);

            KT_CHECK_INT_EQ(n1, n2);
            if (strcmp(g_buffer, g_reference) != 0) {
                KT_CHECK_STR_EQ(g_buffer, g_reference);
                break;
            }
        }
    }

    teardown();
}

/*
 * The property: the data that goes through the calls of the system comes
 * back the same, no matter in which pieces it was written and read.
 */
static void test_write_and_read(void)
{
    static const int chunks[] = {1, 3, 7, 64, 512, 1024, 4096};
    const int nchunks = (int)(sizeof(chunks) / sizeof(chunks[0]));
    char text[8192];
    char back[8192];
    int fd;
    int c;

    ktest_begin("libc: the data comes back the same, whatever the size of the pieces");

    setup();
    load_corpus();
    KT_CHECK(g_count > 0);

    /* The text is the corpus, written as it is. */
    {
        size_t used = 0;
        int n;

        for (n = 0; n < g_count; n++) {
            size_t len = strlen(g_lines[n]);

            if (used + len + 2 >= sizeof(text)) {
                break;
            }
            memcpy(text + used, g_lines[n], len);
            used += len;
            text[used++] = '\n';
        }
        text[used] = '\0';

        fd = microos_open("/libc.txt", MICROOS_O_WRONLY | MICROOS_O_CREAT | MICROOS_O_TRUNC);
        KT_CHECK(fd >= 0);
        KT_CHECK(microos_write(fd, text, used) == (long)used);
        KT_CHECK(microos_close(fd) == 0);
    }

    for (c = 0; c < nchunks; c++) {
        size_t total = 0;
        size_t offset = 0;

        fd = microos_open("/libc.txt", MICROOS_O_RDONLY);
        KT_CHECK(fd >= 0);

        for (;;) {
            long got = microos_read(fd, back + offset, (size_t)chunks[c]);

            if (got <= 0) {
                break;
            }
            offset += (size_t)got;
            total = offset;
            if (offset >= sizeof(back) - 1) {
                break;
            }
        }

        KT_CHECK(microos_close(fd) == 0);
        KT_CHECK(total > 0);
        KT_CHECK_INT_EQ(memcmp(back, text, total), 0);
    }

    teardown();
}

/*
 * The property: what the library prints into a file is the same as what
 * the library of the system prints into a string.
 */
static void test_formatted_output_to_a_file(void)
{
    int fd;
    int n1;
    int n2;
    int i;

    ktest_begin("libc: the formatted output is the same as the one of the system");

    setup();
    load_corpus();

    fd = microos_open("/libc.txt", MICROOS_O_WRONLY | MICROOS_O_CREAT | MICROOS_O_TRUNC);
    KT_CHECK(fd >= 0);

    for (i = 0; i < g_count; i++) {
        char text[LINE_MAX];

        snprintf(text, sizeof(text), "line %d: %s", i, g_lines[i]);

        n1 = microos_fprintf(fd, "%s\n", text);
        n2 = sys_snprintf(g_reference, sizeof(g_reference), "%s\n", text);
        KT_CHECK_INT_EQ(n1, n2);
    }

    KT_CHECK(microos_close(fd) == 0);

    /* Read it back and compare it with the reference. */
    {
        char back[16384];
        size_t total = 0;

        fd = microos_open("/libc.txt", MICROOS_O_RDONLY);
        KT_CHECK(fd >= 0);

        for (;;) {
            long got = microos_read(fd, back + total, sizeof(back) - total - 1);

            if (got <= 0) {
                break;
            }
            total += (size_t)got;
        }
        KT_CHECK(microos_close(fd) == 0);
        back[total] = '\0';

        /* The reference is the same text, made by the library of the system. */
        size_t expected = 0;

        for (i = 0; i < g_count; i++) {
            char text[LINE_MAX];
            int n;

            snprintf(text, sizeof(text), "line %d: %s", i, g_lines[i]);
            n = sys_snprintf(g_reference + expected, sizeof(g_reference) - expected, "%s\n", text);
            expected += (size_t)n;
        }

        KT_CHECK_INT_EQ(total, expected);
        KT_CHECK_INT_EQ(memcmp(back, g_reference, total), 0);
    }

    teardown();
}

/*
 * The property: the errors are the errors of POSIX, and the same call
 * always gives the same error.
 */
static void test_errors(void)
{
    int fd;

    ktest_begin("libc: the errors are the codes of POSIX");

    setup();

    /* The file that is not there. */
    fd = microos_open("/nothing-here", MICROOS_O_RDONLY);
    KT_CHECK_INT_EQ(fd, -1);
    KT_CHECK_INT_EQ(microos_errno(), 2); /* ENOENT */

    /* The same call gives the same error. */
    fd = microos_open("/nothing-here", MICROOS_O_RDONLY);
    KT_CHECK_INT_EQ(fd, -1);
    KT_CHECK_INT_EQ(microos_errno(), 2);

    /* The descriptor that is not open. */
    KT_CHECK_INT_EQ(microos_close(42), -1);
    KT_CHECK_INT_EQ(microos_errno(), 9); /* EBADF */

    KT_CHECK(microos_read(42, g_buffer, 10) < 0);
    KT_CHECK_INT_EQ(microos_errno(), 9);

    /* The directory that is not there. */
    KT_CHECK_INT_EQ(microos_mkdir("/no-such-dir/file"), -1);
    KT_CHECK_INT_EQ(microos_errno(), 2);

    /* The text of the error is the usual one. */
    KT_CHECK_STR_EQ(microos_strerror(2), "no such file or directory");
    KT_CHECK_STR_EQ(microos_strerror(9), "bad file descriptor");
    KT_CHECK_STR_EQ(microos_strerror(0), "no error");

    teardown();
}

/*
 * The property: the console of the system accepts the output of the
 * library and counts the characters.
 */
static void test_console(void)
{
    int n;

    ktest_begin("libc: the console takes the output of the library");

    setup();

    n = microos_fprintf(1, "%s", "");
    KT_CHECK_INT_EQ(n, 0);

    /* The number of the characters is the number that was asked for. */
    n = microos_snprintf(g_buffer, sizeof(g_buffer), "%d-%s-%.2f", 42, "text", 1.5);
    KT_CHECK_INT_EQ(n, (int)strlen(g_buffer));
    KT_CHECK_INT_EQ(strcmp(g_buffer, "42-text-1.50"), 0);

    teardown();
}

void test_libc(void)
{
    test_float_matches_the_system();
    test_integer_matches_the_system();
    test_string_matches_the_system();
    test_write_and_read();
    test_formatted_output_to_a_file();
    test_errors();
    test_console();
}
