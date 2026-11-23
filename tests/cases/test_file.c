/*
 * Micro OS — the tests of file and libmagic, the type of a file.
 *
 * The library is file 5.45 of `third_party/file` (BSD-2-Clause), compiled for
 * the host with the port layer of `third_party/file/port` and the headers of
 * the system. The database is the compact one this build makes: a subset of
 * the Magdir files, compiled by `microos_magic_compile` into `magic.mgc`,
 * with the text beside it. The two levels of the port are covered here: a
 * `magic_load` of the compiled file and of the text.
 *
 * The bytes of each case are the headers a file of the format has at its
 * start, made by the test and not read from the tree: an ELF image, a gzip
 * stream, a Zstandard frame, a PNG image, a tar archive, a MOX1 image of this
 * system, the empty file and a text. The guest build is the same library and
 * the same database in `/bin/file`; `make x86-smoke` runs it on files of the
 * image and `make x86-disk-smoke` on a volume after a reset.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <magic.h>

/* The internal header of the library: `file_buffer` is not in magic.h. */
#include "file.h"

#ifndef MICROOS_TEST_MAGIC
#error "the build has to name the magic database (MICROOS_TEST_MAGIC)"
#endif
#ifndef MICROOS_TEST_MAGIC_TEXT
#error "the build has to name the text of the magic database (MICROOS_TEST_MAGIC_TEXT)"
#endif

#define FILE_TEST_BYTES_MAX 512u

static const unsigned char g_elf_s[] = {
    /* e_ident: the magic, 64-bit, little-endian, version 1, SYSV. */
    0x7f,
    'E',
    'L',
    'F',
    2,
    1,
    1,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    /* e_type: 2 is ET_EXEC, e_machine: 62 is x86-64, e_version: 1. */
    2,
    0,
    62,
    0,
    1,
    0,
    0,
    0,
    /* e_entry, e_phoff, e_shoff, e_flags, e_ehsize, ... */
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    64,
    0,
    56,
    0,
};

/* The first ten bytes of a gzip stream: the magic, deflate, no flags and the
 * zero mtime of a reproducible build. */
static const unsigned char g_gzip_s[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
};

/* A Zstandard frame: the magic of v0.8+ and the first bytes of a block. */
static const unsigned char g_zstd_s[] = {
    0x28, 0xb5, 0x2f, 0xfd, 0x00, 0x58, 0x00, 0x00,
};

/* The signature and the IHDR of a 1x1 RGBA PNG. */
static const unsigned char g_png_s[] = {
    0x89, 'P',  'N',  'G',  0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
    0x0d, 'I',  'H',  'D',  'R',  0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89,
};

/* The header of a MOX1 image: the magic, version 2, no flags, the entry, the
 * sizes and the machine (0 is x86-64). */
static unsigned char g_mox_s[] = {
    'M', 'O', 'X', '1', 2, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0,
    4,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0,
};

static const char g_text_s[] = "hello, Micro OS\n";

/* A tar header of 512 bytes: the name, the mode, the checksum space and the
 * ustar magic at 257, which is what the database reads. */
static unsigned char g_tar_s[512];

static void make_tar(void)
{
    memset(g_tar_s, 0, sizeof(g_tar_s));
    (void)memcpy(g_tar_s, "hello.txt", 9);
    (void)memcpy(g_tar_s + 100, "0000644", 7);
    (void)memcpy(g_tar_s + 108, "0000000", 7);
    (void)memcpy(g_tar_s + 116, "0000000", 7);
    (void)memcpy(g_tar_s + 124, "00000000012", 11);
    (void)memset(g_tar_s + 148, ' ', 8);
    g_tar_s[156] = '0';
    /* The GNU form of the header: "ustar  \0" at 257, which the database
     * names as a POSIX tar archive of GNU. */
    (void)memcpy(g_tar_s + 257, "ustar  ", 7);
    g_tar_s[264] = 0;
    (void)memcpy(g_tar_s + 265, "root", 4);
    (void)memcpy(g_tar_s + 297, "root", 4);
}

/* The description and the MIME type of one buffer, through the two calls of
 * the public header and the internal `file_buffer`. The answers are the
 * fields of a structure of the test and not the buffer of the library, which
 * the next call reuses. */
struct answer {
    char description[256];
    char mime[128];
};

static int answer_of(magic_t cookie, const void *bytes, size_t length, struct answer *out)
{
    const char *text;

    memset(out, 0, sizeof(*out));

    text = magic_buffer(cookie, bytes, length);
    if (text == NULL) {
        return -1;
    }
    (void)snprintf(out->description, sizeof(out->description), "%s", text);

    if (magic_setflags(cookie, MAGIC_MIME) != 0) {
        return -1;
    }
    text = magic_buffer(cookie, bytes, length);
    (void)magic_setflags(cookie, MAGIC_NONE);
    if (text == NULL) {
        return -1;
    }
    (void)snprintf(out->mime, sizeof(out->mime), "%s", text);
    return 0;
}

static void check_one(magic_t cookie, const char *what, const void *bytes, size_t length,
                      const char *description, const char *mime)
{
    struct answer answer;
    int rc = answer_of(cookie, bytes, length, &answer);

    (void)what;
    KT_CHECK(rc == 0);
    if (rc != 0) {
        return;
    }
    KT_CHECK(strstr(answer.description, description) != NULL);
    KT_CHECK(strstr(answer.mime, mime) != NULL);
}

void test_file(void)
{
    magic_t cookie;
    magic_t text_cookie;
    struct answer answer;

    make_tar();

    cookie = magic_open(MAGIC_NONE);
    KT_CHECK(cookie != NULL);
    if (cookie == NULL) {
        return;
    }

    /* The compiled database of the build: the first level of the port. */
    KT_CHECK_INT_EQ(magic_load(cookie, MICROOS_TEST_MAGIC), 0);

    check_one(cookie, "elf", g_elf_s, sizeof(g_elf_s), "ELF 64-bit LSB executable",
              "application/x-executable");
    check_one(cookie, "gzip", g_gzip_s, sizeof(g_gzip_s), "gzip compressed data",
              "application/gzip");
    check_one(cookie, "zstd", g_zstd_s, sizeof(g_zstd_s), "Zstandard compressed data",
              "application/zstd");
    check_one(cookie, "png", g_png_s, sizeof(g_png_s), "PNG image data", "image/png");
    check_one(cookie, "tar", g_tar_s, sizeof(g_tar_s), "POSIX tar archive", "application/x-gtar");
    check_one(cookie, "mox", g_mox_s, sizeof(g_mox_s), "Micro OS MOX1 executable",
              "application/x-microos-mox");
    check_one(cookie, "empty", g_text_s, 0, "empty", "application/x-empty");
    check_one(cookie, "text", g_text_s, strlen(g_text_s), "ASCII text", "text/plain");

    /* The internal call of the library that the program uses for a buffer:
     * it fills the answer of the same structure (the smoke test of the guest
     * is the second reader). */
    KT_CHECK_INT_EQ(file_buffer(cookie, -1, NULL, "text", g_text_s, strlen(g_text_s)), 1);
    KT_CHECK(strstr(file_getbuffer(cookie), "ASCII text") != NULL);

    /* A name that is not a magic database is the honest refusal of the
     * library: the program prints `cannot open magic database` for it. */
    KT_CHECK_INT_EQ(magic_load(cookie, "/no/such/magic"), -1);
    KT_CHECK(magic_error(cookie) != NULL);

    magic_close(cookie);

    /* The text fallback: a second cookie loads the text of the database
     * (the copy without a .mgc beside it), which is the second level of the
     * port and the parser of the release. */
    text_cookie = magic_open(MAGIC_NONE);
    KT_CHECK(text_cookie != NULL);
    if (text_cookie == NULL) {
        return;
    }
    KT_CHECK_INT_EQ(magic_load(text_cookie, MICROOS_TEST_MAGIC_TEXT), 0);
    KT_CHECK_INT_EQ(answer_of(text_cookie, g_png_s, sizeof(g_png_s), &answer), 0);
    KT_CHECK(strstr(answer.description, "PNG image data") != NULL);
    KT_CHECK_INT_EQ(answer_of(text_cookie, g_mox_s, sizeof(g_mox_s), &answer), 0);
    KT_CHECK(strstr(answer.description, "Micro OS MOX1 executable") != NULL);
    magic_close(text_cookie);
}
