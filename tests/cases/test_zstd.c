/*
 * Micro OS — the tests of zstd, the third program library of somebody else.
 *
 * The library is the upstream 1.5.7 of `third_party/zstd` — lib/common,
 * lib/compress and lib/decompress — compiled for the host with its port header
 * (`port/zstd_port.h`: no assembly of the Huffman decoder, no trace, no legacy
 * decoder). The test covers what the command of the guest leans on: the round
 * trip of text and of binary data at the three levels a person uses (1, 3 and
 * 19), the same through the streaming calls the CLI reads a file with, the
 * refusal of a stream that is not a frame, of a truncated one and of one a
 * byte of which was changed, and the two answers about the size of a frame.
 *
 * The guest build is the same source linked into /bin/zstd by the compiler of
 * the window; `make x86-smoke` packs and unpacks a file of the image with it,
 * and the fixture of `/etc/setting.zst` is a frame the `zstd` of the host
 * wrote. The two tests are the two machines of one port.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zstd.h>

#define ZSTD_TEST_INPUT_MAX 65536u
#define ZSTD_TEST_BUFFER_MAX (ZSTD_TEST_INPUT_MAX * 2u)
#define ZSTD_TEST_LEVELS {1, 3, 19}

static unsigned char g_input_s[ZSTD_TEST_INPUT_MAX];
static unsigned char g_packed_s[ZSTD_TEST_BUFFER_MAX];
static unsigned char g_output_s[ZSTD_TEST_INPUT_MAX];

/* The data of the round trip: zeros, a counter, a sentence and a generator of
 * the test (not rand(): the same bytes on every machine and every run). */
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

/* One compression of the whole buffer and one decompression of the frame. */
static int roundtrip(size_t length, int level, unsigned pattern)
{
    size_t packed;
    size_t unpacked;

    fill_input(length, pattern);

    packed = ZSTD_compress(g_packed_s, sizeof(g_packed_s), g_input_s, length, level);
    if (ZSTD_isError(packed)) {
        printf("zstd: level %d: %s\n", level, ZSTD_getErrorName(packed));
        return 1;
    }

    unpacked = ZSTD_decompress(g_output_s, sizeof(g_output_s), g_packed_s, packed);
    if (ZSTD_isError(unpacked)) {
        printf("zstd: level %d: %s\n", level, ZSTD_getErrorName(unpacked));
        return 1;
    }

    if (unpacked != length || memcmp(g_output_s, g_input_s, length) != 0) {
        printf("zstd: level %d: %u bytes came back of %u\n", level, (unsigned)unpacked,
               (unsigned)length);
        return 1;
    }

    return 0;
}

static void test_roundtrip_levels(void)
{
    const int levels[] = ZSTD_TEST_LEVELS;
    const size_t lengths[] = {0, 1, 64, 4096, ZSTD_TEST_INPUT_MAX};
    size_t level_index;
    size_t length_index;
    unsigned pattern;

    for (level_index = 0; level_index < sizeof(levels) / sizeof(levels[0]); level_index++) {
        for (length_index = 0; length_index < sizeof(lengths) / sizeof(lengths[0]);
             length_index++) {
            for (pattern = 0; pattern < 4; pattern++) {
                KT_CHECK_INT_EQ(roundtrip(lengths[length_index], levels[level_index], pattern), 0);
            }
        }
    }
}

/* The streaming calls of the CLI: a frame written in pieces of a buffer and
 * read back in pieces of another one, which is how a file of this system is
 * read (KSYS_MAX_IO is the largest piece of a call). */
static void test_streaming(void)
{
    const int levels[] = ZSTD_TEST_LEVELS;
    size_t level_index;

    for (level_index = 0; level_index < sizeof(levels) / sizeof(levels[0]); level_index++) {
        ZSTD_CCtx *cctx = ZSTD_createCCtx();
        ZSTD_DCtx *dctx = ZSTD_createDCtx();
        size_t const chunk = 4096;
        size_t offset = 0;
        size_t packed = 0;
        size_t unpacked = 0;
        size_t result = 1;
        int level = levels[level_index];

        KT_CHECK(cctx != NULL);
        KT_CHECK(dctx != NULL);
        if (cctx == NULL || dctx == NULL) {
            ZSTD_freeCCtx(cctx);
            ZSTD_freeDCtx(dctx);
            continue;
        }

        fill_input(40000, 2);
        KT_CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level)));

        while (offset < 40000) {
            size_t take = (40000 - offset < chunk) ? 40000 - offset : chunk;
            ZSTD_inBuffer in = {g_input_s + offset, take, 0};
            int last = (offset + take == 40000);

            offset += take;
            do {
                ZSTD_outBuffer out = {g_packed_s + packed, sizeof(g_packed_s) - packed, 0};

                result = ZSTD_compressStream2(cctx, &out, &in, last ? ZSTD_e_end : ZSTD_e_continue);
                KT_CHECK(!ZSTD_isError(result));
                if (ZSTD_isError(result)) {
                    offset = 40000;
                    break;
                }
                packed += out.pos;
            } while (in.pos < in.size || (last && result != 0));
        }

        offset = 0;
        while (offset < packed) {
            size_t take = (packed - offset < chunk) ? packed - offset : chunk;
            ZSTD_inBuffer in = {g_packed_s + offset, take, 0};

            offset += take;
            do {
                ZSTD_outBuffer out = {g_output_s + unpacked, sizeof(g_output_s) - unpacked, 0};

                result = ZSTD_decompressStream(dctx, &out, &in);
                KT_CHECK(!ZSTD_isError(result));
                if (ZSTD_isError(result)) {
                    offset = packed;
                    break;
                }
                unpacked += out.pos;
            } while (in.pos < in.size);
        }

        KT_CHECK_INT_EQ((int)unpacked, 40000);
        KT_CHECK(memcmp(g_output_s, g_input_s, 40000) == 0);

        ZSTD_freeCCtx(cctx);
        ZSTD_freeDCtx(dctx);
    }
}

/* A frame of the library with its checksum, so that a changed byte is a
 * failure of the stream and not only a change of the data. */
static size_t make_frame(int level, int checksum)
{
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    size_t packed = 0;

    fill_input(8192, 3);

    KT_CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level)));
    KT_CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, checksum)));

    packed = ZSTD_compress2(cctx, g_packed_s, sizeof(g_packed_s), g_input_s, 8192);
    ZSTD_freeCCtx(cctx);

    KT_CHECK(!ZSTD_isError(packed));

    return ZSTD_isError(packed) ? 0 : packed;
}

static void test_refusals(void)
{
    size_t packed;
    size_t answer;

    /* Not a frame at all: the text of the machine, a file that is a script
     * where a stream was expected. */
    answer = ZSTD_decompress(g_output_s, sizeof(g_output_s), "# /etc/setting\n", 14);
    KT_CHECK(ZSTD_isError(answer));
    KT_CHECK_INT_EQ((int)ZSTD_getErrorCode(answer), (int)ZSTD_error_prefix_unknown);

    /* A frame it wrote, cut short: the declarations of the stream promise
     * more bytes than are there. */
    packed = make_frame(3, 0);
    KT_CHECK(packed > 8);
    answer = ZSTD_decompress(g_output_s, sizeof(g_output_s), g_packed_s, packed - 4);
    KT_CHECK(ZSTD_isError(answer));

    /* A byte of the compressed data changed, with the checksum of the frame
     * on: the stream is refused and not silently different. */
    packed = make_frame(3, 1);
    KT_CHECK(packed > 16);
    g_packed_s[packed / 2] ^= 0x40u;
    answer = ZSTD_decompress(g_output_s, sizeof(g_output_s), g_packed_s, packed);
    KT_CHECK(ZSTD_isError(answer));
}

static void test_content_size(void)
{
    size_t packed;
    size_t unpacked;

    /* A frame of the one-shot call names the size of the source, and the
     * command of the guest reads it to print the ratio of `-l`. */
    packed = make_frame(1, 0);
    KT_CHECK_INT_EQ((int)ZSTD_getFrameContentSize(g_packed_s, packed), 8192);

    /* A streaming frame of an unknown source names no size: the answer is
     * the one the format keeps for it. */
    {
        ZSTD_CCtx *cctx = ZSTD_createCCtx();
        ZSTD_inBuffer in = {g_input_s, 1024, 0};
        ZSTD_outBuffer out = {g_packed_s, sizeof(g_packed_s), 0};
        size_t rc;

        KT_CHECK(cctx != NULL);
        rc = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue);
        KT_CHECK(!ZSTD_isError(rc));
        in.pos = 0;
        in.size = 0;
        do {
            rc = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
            KT_CHECK(!ZSTD_isError(rc));
        } while (rc != 0);
        ZSTD_freeCCtx(cctx);

        KT_CHECK_INT_EQ((int)ZSTD_getFrameContentSize(g_packed_s, out.pos),
                        (int)ZSTD_CONTENTSIZE_UNKNOWN);

        /* The bytes are still a frame, and they still come back whole. */
        unpacked = ZSTD_decompress(g_output_s, sizeof(g_output_s), g_packed_s, out.pos);
        KT_CHECK(!ZSTD_isError(unpacked));
        KT_CHECK_INT_EQ((int)unpacked, 1024);
    }
}

void test_zstd(void)
{
    ktest_begin("zstd");

    KT_CHECK(strncmp(ZSTD_versionString(), "1.5.", 4) == 0);

    test_roundtrip_levels();
    test_streaming();
    test_refusals();
    test_content_size();
}
