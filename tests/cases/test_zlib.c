/*
 * Micro OS — the tests of zlib, the first program library of somebody else.
 *
 * The library is the upstream 1.3.1 of `third_party/zlib`, compiled for the
 * host with its port layer (`port/zlib_os.c`) and the headers of the system.
 * The test covers the three shapes of the data: the checksums on the vector
 * every zlib port starts from, the round trip of `deflate`/`inflate` in the
 * three wrappers (zlib, raw and gzip), and the streams of a file through the
 * `gz*` layer.
 *
 * The guest build is the same source linked into /bin/zcat by the compiler of
 * the window; `make x86-smoke` inflates a fixture of the image and checks
 * every byte of it. The two tests are the two machines of one port.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <zlib.h>

#define ZLIB_INPUT_MAX 65536u
#define ZLIB_BUFFER_MAX (ZLIB_INPUT_MAX + 4096u)

static unsigned char g_input_s[ZLIB_INPUT_MAX];
static unsigned char g_compressed_s[ZLIB_BUFFER_MAX];
static unsigned char g_output_s[ZLIB_BUFFER_MAX];

/* The data of the round trip: zeros, a counter, a sentence and a generator.
 * The generator is a linear one of the test and not rand(): the same bytes
 * on every machine and every run, which is what a fixture needs. */
static void fill_input(size_t length, unsigned pattern)
{
    size_t index;
    unsigned state = 0x12345678u;

    for (index = 0; index < length; index++) {
        switch (pattern) {
        case 0:
            g_input_s[index] = 0;
            break;
        case 1:
            g_input_s[index] = (unsigned char)(index & 0xffu);
            break;
        case 2:
            g_input_s[index] =
                (unsigned char)"the quick brown fox jumps over the lazy dog\n"[index % 45u];
            break;
        default:
            state = state * 1664525u + 1013904223u;
            g_input_s[index] = (unsigned char)(state >> 24);
            break;
        }
    }
}

static int deflate_inflate_roundtrip(size_t length, int level, int window_bits)
{
    z_stream stream;
    uLong compressed;
    int rc;

    memset(&stream, 0, sizeof(stream));
    rc = deflateInit2(&stream, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY);
    KT_CHECK_INT_EQ(rc, Z_OK);
    if (rc != Z_OK) {
        return 0;
    }

    stream.next_in = g_input_s;
    stream.avail_in = (uInt)length;
    stream.next_out = g_compressed_s;
    stream.avail_out = (uInt)sizeof(g_compressed_s);

    rc = deflate(&stream, Z_FINISH);
    KT_CHECK_INT_EQ(rc, Z_STREAM_END);
    compressed = stream.total_out;
    KT_CHECK(deflateEnd(&stream) == Z_OK);

    if (rc != Z_STREAM_END) {
        return 0;
    }

    memset(&stream, 0, sizeof(stream));
    rc = inflateInit2(&stream, window_bits);
    KT_CHECK_INT_EQ(rc, Z_OK);
    if (rc != Z_OK) {
        return 0;
    }

    stream.next_in = g_compressed_s;
    stream.avail_in = (uInt)compressed;
    stream.next_out = g_output_s;
    stream.avail_out = (uInt)sizeof(g_output_s);

    rc = inflate(&stream, Z_FINISH);
    KT_CHECK_INT_EQ(rc, Z_STREAM_END);
    KT_CHECK_INT_EQ(stream.total_out, (uLong)length);
    KT_CHECK(inflateEnd(&stream) == Z_OK);

    if (rc != Z_STREAM_END || stream.total_out != (uLong)length) {
        return 0;
    }

    KT_CHECK(memcmp(g_input_s, g_output_s, length) == 0);
    KT_CHECK_INT_EQ(crc32(0L, g_input_s, (uInt)length), crc32(0L, g_output_s, (uInt)length));

    return 1;
}

static void test_checksums(void)
{
    static const char check[] = "123456789";

    /* The check values of the zlib manual: every port of the library is
     * compared with the same two numbers. */
    KT_CHECK_INT_EQ(crc32(0L, Z_NULL, 0), 0u);
    KT_CHECK_INT_EQ(crc32(0L, (const Bytef *)check, 9), 0xCBF43926u);
    KT_CHECK_INT_EQ(adler32(0L, Z_NULL, 0), 1u);
    KT_CHECK_INT_EQ(adler32(1L, (const Bytef *)check, 9), 0x091E01DEu);
    KT_CHECK_INT_EQ(crc32(0L, (const Bytef *)"The quick brown fox jumps over the lazy dog", 43),
                    0x414FA339u);
    KT_CHECK_INT_EQ(adler32(1L, (const Bytef *)"Wikipedia", 9), 0x11E60398u);
}

static void test_compress(void)
{
    static const size_t sizes[] = {0, 1, 2, 7, 64, 1024, 8191, 65536};
    static const int levels[] = {0, 1, 6, 9};
    size_t size_index;
    size_t level_index;

    for (size_index = 0; size_index < sizeof(sizes) / sizeof(sizes[0]); size_index++) {
        size_t length = sizes[size_index];

        for (level_index = 0; level_index < sizeof(levels) / sizeof(levels[0]); level_index++) {
            uLongf compressed = sizeof(g_compressed_s);
            uLongf output = sizeof(g_output_s);
            int rc;

            fill_input(length, (unsigned)(size_index + level_index));

            rc = compress2(g_compressed_s, &compressed, g_input_s, (uLong)length,
                           levels[level_index]);
            KT_CHECK_INT_EQ(rc, Z_OK);
            KT_CHECK(compressed <= compressBound((uLong)length));

            rc = uncompress(g_output_s, &output, g_compressed_s, compressed);
            KT_CHECK_INT_EQ(rc, Z_OK);
            KT_CHECK_INT_EQ(output, (uLongf)length);
            KT_CHECK(memcmp(g_input_s, g_output_s, length) == 0);
        }
    }
}

static void test_memory(void)
{
    /* The three wrappers of a stream: the zlib header (15), the raw deflate
     * of a .gz body (-15) and the gzip wrapper of the whole file (31). The
     * empty stream is the edge zlib is easy to get wrong at. */
    static const size_t sizes[] = {0, 1, 3, 17, 255, 4096, 65536};

    for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
        fill_input(sizes[index], (unsigned)index);
        KT_CHECK(deflate_inflate_roundtrip(sizes[index], 6, 15));
        KT_CHECK(deflate_inflate_roundtrip(sizes[index], 1, -15));
        KT_CHECK(deflate_inflate_roundtrip(sizes[index], 9, 31));
    }

    /* A stream that is not a zlib one is refused, and the refusal is an
     * error of the library and not a read past the buffer. */
    {
        z_stream stream;
        static const unsigned char garbage[] = {0x78, 0x9c, 0x00, 0x00, 0x00};

        memset(&stream, 0, sizeof(stream));
        KT_CHECK_INT_EQ(inflateInit(&stream), Z_OK);
        stream.next_in = (Bytef *)garbage;
        stream.avail_in = sizeof(garbage);
        stream.next_out = g_output_s;
        stream.avail_out = sizeof(g_output_s);
        KT_CHECK(inflate(&stream, Z_FINISH) != Z_STREAM_END);
        KT_CHECK(inflateEnd(&stream) == Z_OK);
    }
}

static void test_gz_file(void)
{
    static const char message[] =
        "zlib in Micro OS: a stream of the VFS, not of the libc of the host.\n"
        "the second line is here so that a partial read is visible.\n";
    char path[] = "/tmp/microos-zlib-XXXXXX";
    int fd;
    gzFile stream;
    char buffer[256];
    size_t total = 0;
    int rc = 0;

    fd = mkstemp(path);
    KT_CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }

    stream = gzdopen(fd, "wb");
    KT_CHECK(stream != NULL);
    if (stream == NULL) {
        (void)close(fd);
        (void)unlink(path);
        return;
    }

    KT_CHECK_INT_EQ(gzwrite(stream, message, (unsigned)(sizeof(message) - 1)),
                    (int)(sizeof(message) - 1));
    KT_CHECK_INT_EQ(gzclose(stream), Z_OK);

    stream = gzopen(path, "rb");
    KT_CHECK(stream != NULL);
    if (stream != NULL) {
        while (total < sizeof(buffer) && (rc = gzread(stream, buffer + total, 7)) > 0) {
            total += (size_t)rc;
        }
        KT_CHECK_INT_EQ(rc, 0);
        KT_CHECK(gzeof(stream));
        KT_CHECK_INT_EQ(total, sizeof(message) - 1);
        KT_CHECK(memcmp(buffer, message, total) == 0);
        KT_CHECK_INT_EQ(gzclose(stream), Z_OK);
    }

    KT_CHECK(gzopen("/no/such/file-zlib-test", "rb") == NULL);
    KT_CHECK(unlink(path) == 0);
}

void test_zlib(void)
{
    ktest_begin("zlib");

    KT_CHECK(strncmp(zlibVersion(), "1.3.", 4) == 0);

    test_checksums();
    test_compress();
    test_memory();
    test_gz_file();
}
