/*
 * Micro OS — the parser of the program format against junk.
 *
 * This is not libFuzzer: the suite has no dependencies and a CI machine that
 * runs the tests twice has to see the same numbers. A fixed seed and a small
 * xorshift make the corpus reproducible, and the cases that matter are spelled
 * out by hand: a header whose fields sit on a boundary, an image cut in the
 * middle, thousands of buffers that are not an image at all.
 *
 * The parser promises two things that are checked here: it never writes the
 * output of a refused image, and whatever it accepts is a consistent image.
 * A buffer of noise is not an image, so the random half must accept none of
 * them; the header half builds its bytes from the magic and the fields, and
 * there an acceptance is fine as long as the result holds together.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/exec.h"
#include "kernel/kmm.h"
#include "kernel/port/exec.h"

#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

/* A seed of its own: the fuzz of the parser is not the fuzz of another test. */
#define FUZZ_SEED 0x5EEDF00Du
#define FUZZ_BUFFER 256u
#define FUZZ_RANDOM_ITERATIONS 3000u
#define FUZZ_HEADER_ITERATIONS 2000u
#define FUZZ_WORDS_MAX 48

/* The image the targeted cases start from: four bytes of code, four of data. */
#define CASE_TEXT_BYTES 4u
#define CASE_DATA_BYTES 4u
#define CASE_IMAGE_SIZE (K_EXEC_HEADER_SIZE + CASE_TEXT_BYTES + CASE_DATA_BYTES)

static k_u32_t g_rng = FUZZ_SEED;
static k_u32_t g_words[FUZZ_WORDS_MAX];
static int g_word_count;

static k_u32_t next_u32(void)
{
    /* xorshift32: three shifts and three xor, and the sequence is fixed. */
    k_u32_t value = g_rng;

    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    g_rng = value;

    return value;
}

static void put_u32(k_u8_t *at, k_u32_t value)
{
    at[0] = (k_u8_t)(value & 0xFFu);
    at[1] = (k_u8_t)((value >> 8) & 0xFFu);
    at[2] = (k_u8_t)((value >> 16) & 0xFFu);
    at[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/* A word of a header: a boundary of the corpus, a small number, or noise. */
static k_u32_t pick_word(void)
{
    switch (next_u32() % 3u) {
    case 0:
        return next_u32();
    case 1:
        return g_word_count > 0 ? g_words[next_u32() % (k_u32_t)g_word_count] : next_u32();
    default:
        return next_u32() % 64u;
    }
}

/*
 * The corpus is a list of real numbers with the negative and the boundary
 * values among them. As the sizes and the offsets of a header they are the
 * words that have broken a sum before: 0, 255, 65536, 2^31 and the -1 that is
 * "all bits set" once it reaches a k_u32_t.
 */
static void add_word(k_u32_t word)
{
    if (g_word_count < (int)(sizeof(g_words) / sizeof(g_words[0]))) {
        g_words[g_word_count++] = word;
    }
}

static void load_words(void)
{
    FILE *file;
    char line[128];

    g_word_count = 0;

    file = fopen(KTEST_CORPUS, "r");
    if (file != NULL) {
        while (fgets(line, sizeof(line), file) != NULL) {
            char *end = NULL;
            long long value;

            value = strtoll(line, &end, 10);
            if (end == line) {
                continue;
            }
            add_word((k_u32_t)value);
        }
        fclose(file);
    }

    /* The corpus may be absent on a checkout that runs one test by hand: the
     * boundary words are the point, so they are here and not only in a file. */
    add_word(0);
    add_word(1);
    add_word(2);
    add_word(3);
    add_word(4);
    add_word(31);
    add_word(32);
    add_word(33);
    add_word(255);
    add_word(256);
    add_word(65535);
    add_word(65536);
    add_word(0x7FFFFFFFu);
    add_word(0x80000000u);
    add_word(K_EXEC_BSS_MAX - 4u);
    add_word(K_EXEC_BSS_MAX);
    add_word(K_EXEC_BSS_MAX + 4u);
    add_word(0xFFFFFFFCu);
    add_word(0xFFFFFFFDu);
    add_word(0xFFFFFFFEu);
    add_word(0xFFFFFFFFu);
}

/* What the parser promises about an image it accepts. */
static bool parsed_is_consistent(const struct k_exec_image *image)
{
    k_u64_t text_end;
    k_u64_t end;

    if (image->data == NULL) {
        return false;
    }

    if (image->text_size == 0 || (image->text_size & 3u) != 0) {
        return false;
    }

    if ((image->data_size & 3u) != 0 || (image->bss_size & 3u) != 0) {
        return false;
    }

    if (image->bss_size > K_EXEC_BSS_MAX) {
        return false;
    }

    /* The entry points into the text and nowhere else. */
    text_end = (k_u64_t)K_EXEC_HEADER_SIZE + image->text_size;
    if ((k_u64_t)image->entry < K_EXEC_HEADER_SIZE || (k_u64_t)image->entry >= text_end) {
        return false;
    }

    /* The segments the header names are inside the bytes the caller gave. */
    end = (k_u64_t)K_EXEC_HEADER_SIZE + image->text_size + image->data_size;
    return end <= (k_u64_t)image->size;
}

static void expect_parse(const k_u8_t *image, k_size_t size, int expected)
{
    struct k_exec_image out;
    struct k_exec_image untouched;

    memset(&out, 0xA5, sizeof(out));
    untouched = out;

    KT_CHECK_INT_EQ(k_exec_parse(image, size, &out), expected);

    if (expected == 0) {
        KT_CHECK(parsed_is_consistent(&out));
        KT_CHECK(out.data == image);
        KT_CHECK_INT_EQ(out.size, size);
    } else {
        /* A refused image leaves nothing behind for the caller to read. */
        KT_CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);
    }
}

/* The header of a consistent image: four bytes of text and four of data. */
static void build_case_image(k_u8_t *image)
{
    memset(image, 0, CASE_IMAGE_SIZE);

    put_u32(&image[0], K_EXEC_MAGIC);
    put_u32(&image[4], K_EXEC_VERSION);
    put_u32(&image[8], 0); /* flags */
    put_u32(&image[12], K_EXEC_HEADER_SIZE);
    put_u32(&image[16], CASE_TEXT_BYTES);
    put_u32(&image[20], CASE_DATA_BYTES);
    put_u32(&image[24], 0); /* bss */
    put_u32(&image[28], 0); /* reserved */

    image[K_EXEC_HEADER_SIZE + 0] = 0xC3; /* ret */
    image[K_EXEC_HEADER_SIZE + 1] = 0x90; /* nop */
    image[K_EXEC_HEADER_SIZE + CASE_TEXT_BYTES] = 0x2A;
}

static void check_random_buffers(void)
{
    k_u32_t accepted = 0;
    k_u32_t iteration;

    ktest_begin("fuzz: random bytes are not an image");

    for (iteration = 0; iteration < FUZZ_RANDOM_ITERATIONS; iteration++) {
        struct k_exec_image out;
        struct k_exec_image untouched;
        k_size_t size = (k_size_t)(next_u32() % (FUZZ_BUFFER + 1u));
        k_u8_t *buffer = (k_u8_t *)kmm_alloc(size > 0 ? size : 1u, KMM_PRIORITY_KERNEL);
        k_size_t index;

        KT_CHECK(buffer != NULL);
        if (buffer == NULL) {
            return;
        }

        for (index = 0; index < size; index++) {
            buffer[index] = (k_u8_t)(next_u32() & 0xFFu);
        }

        memset(&out, 0xA5, sizeof(out));
        untouched = out;

        if (k_exec_parse(buffer, size, &out) == 0) {
            accepted++;
            KT_CHECK(parsed_is_consistent(&out));
        } else {
            KT_CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);
        }

        /* The block is exactly the size the parser is told: a byte read past
         * the end is a fault of the sanitizer build and not a quiet success. */
        kmm_free(buffer);
    }

    /* The magic is four exact bytes: noise does not have them by accident,
     * and a parser that accepts noise here is not checking the header. */
    KT_CHECK_INT_EQ(accepted, 0);
}

static void check_random_headers(void)
{
    k_u8_t image[CASE_IMAGE_SIZE];
    k_u32_t accepted = 0;
    k_u32_t iteration;

    ktest_begin("fuzz: a header of noise is accepted only when it holds together");

    for (iteration = 0; iteration < FUZZ_HEADER_ITERATIONS; iteration++) {
        struct k_exec_image out;
        struct k_exec_image untouched;
        k_size_t size;
        k_u32_t corruptions;
        k_u32_t remaining;
        int result;

        /* The base is a consistent image, so a run of no mutation has to be
         * accepted; the rest corrupt one or two fields and cut the file
         * anywhere, which is what a damaged program looks like on the flash. */
        build_case_image(image);

        corruptions = next_u32() % 3u;
        remaining = corruptions;
        while (remaining > 0) {
            k_u32_t field = next_u32() % 8u;

            put_u32(&image[field * 4u], pick_word());
            remaining--;
        }

        size = corruptions == 0 && iteration % 2u == 0
                   ? sizeof(image)
                   : (k_size_t)(next_u32() % (sizeof(image) + 1u));

        memset(&out, 0xA5, sizeof(out));
        untouched = out;

        result = k_exec_parse(image, size, &out);

        if (result == 0) {
            accepted++;
            KT_CHECK(parsed_is_consistent(&out));
        } else {
            KT_CHECK_INT_EQ(result, -1);
            KT_CHECK(memcmp(&out, &untouched, sizeof(out)) == 0);
        }
    }

    /* The loop has to see both answers: a parser that refuses everything
     * would pass the checks above without parsing a single field. */
    KT_CHECK(accepted > 0);
}

static void check_truncated_images(void)
{
    k_u8_t image[CASE_IMAGE_SIZE];
    k_size_t size;

    ktest_begin("fuzz: an image cut anywhere is refused");

    build_case_image(image);

    for (size = 0; size < sizeof(image); size++) {
        expect_parse(image, size, -1);
    }

    expect_parse(image, sizeof(image), 0);

    /* The format allows the padding after the data: a longer file is still
     * the same image, and the parser reads the sizes from the header. */
    {
        k_u8_t padded[CASE_IMAGE_SIZE + 4u];

        memset(padded, 0, sizeof(padded));
        memcpy(padded, image, sizeof(image));
        expect_parse(padded, sizeof(padded), 0);
    }
}

static void check_boundaries(void)
{
    k_u8_t image[CASE_IMAGE_SIZE];

    ktest_begin("fuzz: the fields on a boundary");

    build_case_image(image);
    expect_parse(image, sizeof(image), 0);

    /* The magic and the version are exact, and an unknown generation is a
     * refusal, not a header to guess about. */
    build_case_image(image);
    put_u32(&image[0], K_EXEC_MAGIC + 1u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[0], 0);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[4], 0);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[4], K_EXEC_VERSION + 1u);
    expect_parse(image, sizeof(image), -1);

    /* A flag this generation does not know, and a reserved word that is not
     * zero, are the same thing. */
    build_case_image(image);
    put_u32(&image[8], 1u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[8], 0xFFFFFFFFu);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[28], 1u);
    expect_parse(image, sizeof(image), -1);

    /* A file shorter than the header is not an image at all. */
    build_case_image(image);
    expect_parse(image, K_EXEC_HEADER_SIZE - 1u, -1);
    expect_parse(image, K_EXEC_HEADER_SIZE, -1);

    /* Every size is a multiple of four and the code is never empty. */
    build_case_image(image);
    put_u32(&image[16], 0);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[16], 1u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[16], 2u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[16], 3u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[20], 1u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[20], 3u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[24], 1u);
    expect_parse(image, sizeof(image), -1);

    /* The bss is not in the file: the ceiling is the only bound. */
    build_case_image(image);
    put_u32(&image[24], K_EXEC_BSS_MAX - 4u);
    expect_parse(image, sizeof(image), 0);
    build_case_image(image);
    put_u32(&image[24], K_EXEC_BSS_MAX);
    expect_parse(image, sizeof(image), 0);
    build_case_image(image);
    put_u32(&image[24], K_EXEC_BSS_MAX + 4u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[24], 0xFFFFFFFFu);
    expect_parse(image, sizeof(image), -1);

    /* The entry is an offset into the text, the header and the data are not
     * code; a byte inside the text is the compiler's business, not ours. */
    build_case_image(image);
    put_u32(&image[12], 0);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[12], K_EXEC_HEADER_SIZE - 1u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[12], K_EXEC_HEADER_SIZE);
    expect_parse(image, sizeof(image), 0);
    build_case_image(image);
    put_u32(&image[12], K_EXEC_HEADER_SIZE + 1u);
    expect_parse(image, sizeof(image), 0);
    build_case_image(image);
    put_u32(&image[12], K_EXEC_HEADER_SIZE + CASE_TEXT_BYTES - 1u);
    expect_parse(image, sizeof(image), 0);
    build_case_image(image);
    put_u32(&image[12], K_EXEC_HEADER_SIZE + CASE_TEXT_BYTES);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[12], 0xFFFFFFFFu);
    expect_parse(image, sizeof(image), -1);

    /* A sum that would leave the 32-bit address space is refused by the
     * 64-bit arithmetic and not by a wrapped comparison. */
    build_case_image(image);
    put_u32(&image[16], 0xFFFFFFFCu);
    put_u32(&image[20], 0xFFFFFFFCu);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[16], 0xFFFFFFFCu);
    put_u32(&image[20], 0);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[16], 0xFFFFFFFCu);
    put_u32(&image[12], 0xFFFFFFFCu);
    expect_parse(image, sizeof(image), -1);
}

static void check_run_refusals(void)
{
    k_u8_t image[CASE_IMAGE_SIZE];
    struct k_exec_image parsed;

    ktest_begin("fuzz: k_exec_run refuses a structure that is not an image");

    KT_CHECK_INT_EQ(k_exec_run(NULL, NULL), -1);

    build_case_image(image);
    memset(&parsed, 0, sizeof(parsed));
    KT_CHECK_INT_EQ(k_exec_parse(image, sizeof(image), &parsed), 0);

    /* Nothing to copy: the data is the required field, the sizes come after
     * it, and a zero text is not a program with no code. */
    {
        struct k_exec_image empty = parsed;

        empty.data = NULL;
        KT_CHECK_INT_EQ(k_exec_run(&empty, NULL), -1);

        empty = parsed;
        empty.text_size = 0;
        KT_CHECK_INT_EQ(k_exec_run(&empty, NULL), -1);
    }

    /*
     * The host cannot enter a program at all, and that answer comes before
     * the fit of the window: every structurally valid request gets -2 and
     * no success. The window itself is the business of the ports that can
     * run one, and the x86-64 smoke test covers it on the machine.
     */
    KT_CHECK(!k_port_exec_supported());
    KT_CHECK_INT_EQ(k_exec_run(&parsed, NULL), -2);

    /* A 32-bit size that fills the address space is not a success either. */
    parsed.text_size = (k_size_t)0xFFFFFFFCu;
    KT_CHECK(k_exec_run(&parsed, NULL) < 0);
}

void test_fuzz(void)
{
    kmm_init();
    load_words();
    KT_CHECK(g_word_count > 0);

    g_rng = FUZZ_SEED;

    check_random_buffers();
    check_random_headers();
    check_truncated_images();
    check_boundaries();
    check_run_refusals();
}
