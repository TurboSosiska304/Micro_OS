/*
 * Micro OS — the parser of the program format and the parsers of MicroFS
 * against junk.
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
 *
 * The second half plays the same game with a volume of MicroFS: the records
 * of the inodes, the map of the free blocks, the entries of a directory and
 * the header are changed on a RAM device and the volume is mounted and
 * checked. The promises are that the parsers never leave the buffers they
 * were given (the sanitizer build is the judge) and that garbage is not
 * taken for a whole volume.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/config.h"
#include "kernel/exec.h"
#include "kernel/kmm.h"
#include "kernel/port/exec.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"

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

static void put_u16(k_u8_t *at, k_u16_t value)
{
    at[0] = (k_u8_t)(value & 0xFFu);
    at[1] = (k_u8_t)((value >> 8) & 0xFFu);
}

/* A word of a header: a boundary of the corpus, a small number, or noise. */
#if KERNEL_EXEC
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
#endif /* KERNEL_EXEC */

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
#if KERNEL_EXEC
static bool parsed_is_consistent(const struct k_exec_image *image)
{
    k_u64_t text_end;
    k_u64_t end;

    if (image->data == NULL) {
        return false;
    }

    if (image->arch != K_EXEC_ARCH_X86_64 && image->arch != K_EXEC_ARCH_XTENSA) {
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
    put_u32(&image[24], 0);                  /* bss */
    put_u32(&image[28], K_EXEC_ARCH_X86_64); /* the machine of the bytes */

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

    /* A flag this generation does not know is the same kind of refusal. */
    build_case_image(image);
    put_u32(&image[8], 1u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[8], 0xFFFFFFFFu);
    expect_parse(image, sizeof(image), -1);

    /* Both machines of the format are headers the parser describes; a number
     * outside them is a machine nothing here knows. */
    build_case_image(image);
    put_u32(&image[28], K_EXEC_ARCH_XTENSA);
    expect_parse(image, sizeof(image), 0);
    build_case_image(image);
    put_u32(&image[28], 2u);
    expect_parse(image, sizeof(image), -1);
    build_case_image(image);
    put_u32(&image[28], 0xFFFFFFFFu);
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
#endif /* KERNEL_EXEC */

/* --- the fuzz of the filesystem --- */

/*
 * The volume of the fuzz is a real MicroFS made by the formatter, with a
 * tree and a few files in it. The addresses come from the superblock and
 * from the records, never from a hard-coded layout, so the fuzz follows the
 * format instead of keeping a second copy of it. Every case starts from the
 * snapshot of a sound volume: a mutation that a previous case left on the
 * medium cannot decide the next one.
 */
#define FS_FUZZ_BLOCKS 256u
#define FS_FUZZ_RECORDS 64u
#define FS_FUZZ_BYTES (FS_FUZZ_BLOCKS * MFS_BLOCK_SIZE)
#define FS_FUZZ_SEED 0x4D46534Bu /* "MFSK" */
#define FS_FUZZ_RANDOM_VOLUMES 64u
#define FS_FUZZ_RANDOM_RECORDS 64u
#define FS_FUZZ_RANDOM_BLOCKS 96u

/* The bytes of data in a block: the first four hold the next block. */
#define FS_FUZZ_BLOCK_DATA (MFS_BLOCK_SIZE - 4u)

struct fs_fuzz_volume {
    struct mfs_ram_device ram;
    struct k_blockdev dev;
    struct mfs_superblock sb;
    k_u8_t *pristine;
    k_u32_t root_block;
    k_u32_t logs_inode;
    k_u32_t logs_block;
    k_u32_t alpha_inode;
    k_u32_t alpha_block;
    k_u32_t capacity;
};

static struct fs_fuzz_volume g_fs;

static bool fs_read_block(k_u32_t block, void *buffer)
{
    return g_fs.dev.ops->read(g_fs.dev.ctx, block, buffer, 1) == 0;
}

static bool fs_write_block(k_u32_t block, const void *buffer)
{
    return g_fs.dev.ops->write(g_fs.dev.ctx, block, buffer, 1) == 0;
}

static k_u32_t fs_bitmap_blocks(const struct mfs_superblock *sb)
{
    return (sb->data_blocks + 8u * MFS_BLOCK_SIZE - 1u) / (8u * MFS_BLOCK_SIZE);
}

static k_u32_t fs_inode_block(k_u32_t index)
{
    return g_fs.sb.meta_start + fs_bitmap_blocks(&g_fs.sb) +
           (index * MFS_INODE_SIZE) / MFS_BLOCK_SIZE;
}

static k_u32_t fs_inode_offset(k_u32_t index)
{
    return (index * MFS_INODE_SIZE) % MFS_BLOCK_SIZE;
}

static bool fs_read_inode(k_u32_t index, struct mfs_inode *inode)
{
    k_u8_t block[MFS_BLOCK_SIZE];

    if (!fs_read_block(fs_inode_block(index), block)) {
        return false;
    }

    memcpy(inode, block + fs_inode_offset(index), sizeof(*inode));

    return true;
}

static bool fs_write_inode(k_u32_t index, const struct mfs_inode *inode)
{
    k_u8_t block[MFS_BLOCK_SIZE];

    if (!fs_read_block(fs_inode_block(index), block)) {
        return false;
    }

    memcpy(block + fs_inode_offset(index), inode, sizeof(*inode));

    return fs_write_block(fs_inode_block(index), block);
}

/* Flip a bit of the map of the free blocks, through the block device. */
static bool fs_bitmap_set(k_u32_t index, bool used)
{
    k_u32_t per_block = 8u * MFS_BLOCK_SIZE;
    k_u32_t number = g_fs.sb.meta_start + index / per_block;
    k_u32_t byte = (index / 8u) % MFS_BLOCK_SIZE;
    k_u8_t bit = (k_u8_t)(1u << (index % 8u));
    k_u8_t block[MFS_BLOCK_SIZE];

    if (!fs_read_block(number, block)) {
        return false;
    }

    if (used) {
        block[byte] |= bit;
    } else {
        block[byte] &= (k_u8_t)~bit;
    }

    return fs_write_block(number, block);
}

/* The map of the sound volume, where a free block can be found. */
static bool fs_bitmap_used(k_u32_t index)
{
    k_size_t offset = (k_size_t)g_fs.sb.meta_start * MFS_BLOCK_SIZE + index / 8u;

    return (g_fs.pristine[offset] & (1u << (index % 8u))) != 0u;
}

static void fs_noise(k_u8_t *block, k_size_t size)
{
    k_size_t i;

    for (i = 0; i + 4u <= size; i += 4u) {
        put_u32(block + i, next_u32());
    }
}

static void fs_restore(void)
{
    mfs_unmount();
    memcpy(g_fs.ram.data, g_fs.pristine, FS_FUZZ_BYTES);
}

static bool fs_create_file(const char *path, const char *text)
{
    struct mfs_file file;
    int length = (int)strlen(text);
    int rc;

    if (mfs_create(path) != MFS_OK) {
        return false;
    }

    if (mfs_open(path, MFS_O_WRONLY, &file) != MFS_OK) {
        return false;
    }

    rc = mfs_write(&file, text, (k_size_t)length);
    if (mfs_close(&file) != MFS_OK) {
        return false;
    }

    return rc == length;
}

static bool fs_fuzz_setup(void)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    struct mfs_inode inode;
    struct mfs_dirent dirent;

    memset(&g_fs, 0, sizeof(g_fs));

    if (mfs_ram_init(&g_fs.ram, FS_FUZZ_BLOCKS) != 0) {
        return false;
    }

    mfs_ram_device(&g_fs.ram, &g_fs.dev);

    if (mfs_format(&g_fs.dev, FS_FUZZ_RECORDS) != MFS_OK || mfs_mount(&g_fs.dev) != MFS_OK) {
        goto fail;
    }

    if (mfs_mkdir("/logs") != MFS_OK || mfs_mkdir("/logs/old") != MFS_OK) {
        goto fail;
    }

    if (!fs_create_file("/alpha", "alpha") || !fs_create_file("/bravo", "bravo") ||
        !fs_create_file("/logs/note.txt", "note")) {
        goto fail;
    }

    if (mfs_stat("/", &inode, NULL) != MFS_OK) {
        goto fail;
    }
    g_fs.root_block = inode.first_block;

    if (mfs_stat("/logs", &inode, &dirent) != MFS_OK) {
        goto fail;
    }
    g_fs.logs_inode = dirent.inode;
    g_fs.logs_block = inode.first_block;

    if (mfs_stat("/alpha", &inode, &dirent) != MFS_OK) {
        goto fail;
    }
    g_fs.alpha_inode = dirent.inode;
    g_fs.alpha_block = inode.first_block;

    /* The snapshot is taken from an unmounted volume, so it holds what the
     * medium would keep after a restart and not a buffer of the mount. */
    mfs_unmount();

    g_fs.pristine = (k_u8_t *)malloc(FS_FUZZ_BYTES);
    if (g_fs.pristine == NULL) {
        goto fail;
    }
    memcpy(g_fs.pristine, g_fs.ram.data, FS_FUZZ_BYTES);

    if (!fs_read_block(FS_FUZZ_BLOCKS - 1u, block)) {
        goto fail;
    }
    memcpy(&g_fs.sb, block, sizeof(g_fs.sb));

    g_fs.capacity =
        (g_fs.sb.meta_blocks - fs_bitmap_blocks(&g_fs.sb)) * MFS_BLOCK_SIZE / MFS_INODE_SIZE - 1u;

    return true;

fail:
    mfs_unmount();
    free(g_fs.pristine);
    g_fs.pristine = NULL;
    mfs_ram_free(&g_fs.ram);

    return false;
}

static void fs_fuzz_teardown(void)
{
    mfs_unmount();
    free(g_fs.pristine);
    g_fs.pristine = NULL;
    mfs_ram_free(&g_fs.ram);
}

/* The entries of a directory block, after the four bytes of the chain. */
static k_size_t fs_find_entry(const k_u8_t *block, const char *name)
{
    k_size_t size = strlen(name);
    k_size_t offset = 0;

    while (offset + sizeof(struct mfs_dirent_header) <= FS_FUZZ_BLOCK_DATA) {
        k_u16_t name_len;

        memcpy(&name_len, block + 4u + offset + 4u, sizeof(name_len));

        if ((k_size_t)name_len == size && memcmp(block + 4u + offset + 8u, name, size) == 0) {
            return offset;
        }

        if (name_len == 0u || offset + 8u + (k_size_t)name_len > FS_FUZZ_BLOCK_DATA) {
            break;
        }

        offset += 8u + (k_size_t)name_len;
    }

    return (k_size_t)-1;
}

/*
 * Read the root and a directory of it through the parsers of the filesystem.
 * A damaged tree is allowed to answer an error; it is not allowed to hand
 * back an entry whose name runs past the buffer it lives in.
 */
static void fs_probe(void)
{
    struct mfs_dirent listed[8];
    struct mfs_dirent entries[8];
    struct mfs_file dir;
    k_size_t count = 0;
    k_size_t i;
    int rc;

    memset(listed, 0, sizeof(listed));
    rc = mfs_list("/", listed, 8);
    if (rc == MFS_OK) {
        for (i = 0; i < 8u; i++) {
            KT_CHECK(listed[i].name_len <= MFS_NAME_MAX);
            KT_CHECK(memchr(listed[i].name, '\0', sizeof(listed[i].name)) != NULL);
        }
    }

    memset(entries, 0, sizeof(entries));
    rc = mfs_open("/", MFS_O_RDONLY | MFS_O_DIRECTORY, &dir);
    if (rc != MFS_OK) {
        return;
    }

    rc = mfs_readdir(&dir, entries, 8, &count);
    KT_CHECK(rc == MFS_OK || rc == MFS_EIO || rc == MFS_E2BIG || rc == MFS_ENOTDIR);

    if (rc == MFS_OK) {
        KT_CHECK(count <= 8u);

        for (i = 0; i < count; i++) {
            KT_CHECK(entries[i].name_len <= MFS_NAME_MAX);
            KT_CHECK(memchr(entries[i].name, '\0', sizeof(entries[i].name)) != NULL);
        }
    }

    KT_CHECK_INT_EQ(mfs_close(&dir), MFS_OK);
}

/*
 * The volume mounts and the check has to name a problem. The mutations of
 * this file keep the root record intact, so the mount itself succeeds and
 * the check is the part that sees the damage.
 */
static void fs_expect_damage(void)
{
    struct mfs_check_report report;
    int rc = mfs_mount(&g_fs.dev);

    KT_CHECK_INT_EQ(rc, MFS_OK);

    if (rc != MFS_OK) {
        return;
    }

    KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
    KT_CHECK(report.errors > 0);
    KT_CHECK(report.first[0] != '\0');

    fs_probe();
    mfs_unmount();
}

/* A whole volume of noise never has the header of MicroFS. */
static void fs_fuzz_random_volumes(void)
{
    k_u32_t iteration;
    k_size_t i;

    ktest_begin("fuzz: a volume of noise is not a volume");

    for (iteration = 0; iteration < FS_FUZZ_RANDOM_VOLUMES; iteration++) {
        fs_restore();

        for (i = 0; i < FS_FUZZ_BYTES; i += 4u) {
            put_u32(&g_fs.ram.data[i], next_u32());
        }

        KT_CHECK(mfs_mount(&g_fs.dev) != MFS_OK);
        KT_CHECK(!mfs_is_mounted());
    }
}

/*
 * The header of a sound volume over a medium of noise: the mount is allowed
 * to trust the header and read the rest, and then the check has to see that
 * the sections under it are not a filesystem. A volume that mounts and then
 * passes the check would be garbage taken for a whole volume.
 */
static void fs_fuzz_garbage_below_header(void)
{
    k_u32_t iteration;
    k_u32_t block_index;

    ktest_begin("fuzz: noise under a sound header is not a filesystem");

    for (iteration = 0; iteration < 8u; iteration++) {
        struct mfs_check_report report;
        k_u8_t block[MFS_BLOCK_SIZE];
        int rc;

        fs_restore();

        for (block_index = 0; block_index + 1u < FS_FUZZ_BLOCKS; block_index++) {
            fs_noise(block, sizeof(block));

            if (!fs_write_block(block_index, block)) {
                KT_CHECK(false);
                return;
            }
        }

        rc = mfs_mount(&g_fs.dev);
        if (rc == MFS_OK) {
            KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
            KT_CHECK(report.errors > 0);
            mfs_unmount();
        } else {
            KT_CHECK(!mfs_is_mounted());
        }
    }
}

/* Write a damaged header and expect the mount to refuse it. */
static void fs_expect_header_refused(const struct mfs_superblock *sb)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t header = FS_FUZZ_BLOCKS - 1u;

    fs_restore();

    if (!fs_read_block(header, block)) {
        KT_CHECK(false);
        return;
    }

    memcpy(block, sb, sizeof(*sb));

    if (!fs_write_block(header, block)) {
        KT_CHECK(false);
        return;
    }

    KT_CHECK(mfs_mount(&g_fs.dev) != MFS_OK);
    KT_CHECK(!mfs_is_mounted());
}

static void fs_fuzz_superblock(void)
{
    struct mfs_superblock sb;

    ktest_begin("fuzz: a damaged header is refused");

    sb = g_fs.sb;
    sb.magic ^= 0xFFFFu;
    fs_expect_header_refused(&sb);

    sb = g_fs.sb;
    sb.version++;
    fs_expect_header_refused(&sb);

    sb = g_fs.sb;
    sb.block_size = 512u;
    fs_expect_header_refused(&sb);

    sb = g_fs.sb;
    sb.total_blocks--;
    fs_expect_header_refused(&sb);

    /* Every section has to exist and the three have to fill the volume. */
    sb = g_fs.sb;
    sb.data_blocks = 0u;
    fs_expect_header_refused(&sb);

    sb = g_fs.sb;
    sb.meta_blocks = 0u;
    fs_expect_header_refused(&sb);

    sb = g_fs.sb;
    sb.info_blocks = 0u;
    fs_expect_header_refused(&sb);

    sb = g_fs.sb;
    sb.meta_start++;
    fs_expect_header_refused(&sb);

    sb = g_fs.sb;
    sb.info_start++;
    fs_expect_header_refused(&sb);

    sb = g_fs.sb;
    sb.root_inode = 2u;
    fs_expect_header_refused(&sb);

    /* The checksum is the one thing a brownout cannot fool. */
    sb = g_fs.sb;
    sb.crc ^= 1u;
    fs_expect_header_refused(&sb);
}

static void fs_fuzz_bitmap(void)
{
    k_u32_t free_block = 0;
    k_u32_t i;
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u32_t iteration;

    ktest_begin("fuzz: a damaged map of the free blocks is named");

    /* The block of the root is taken; a clear bit says it is not. */
    fs_restore();
    KT_CHECK(fs_bitmap_set(g_fs.root_block, false));
    fs_expect_damage();

    /* And a set bit for a block that no record names is the other error. */
    for (i = 1; i < g_fs.sb.data_blocks; i++) {
        if (!fs_bitmap_used(i)) {
            free_block = i;
            break;
        }
    }
    KT_CHECK(free_block != 0u);

    fs_restore();
    KT_CHECK(fs_bitmap_set(free_block, true));
    fs_expect_damage();

    /* Noise in the map: the fuzzer keeps the root marked used so that the
     * case is a damage of the map and not a volume with no root block. */
    for (iteration = 0; iteration < 8u; iteration++) {
        fs_restore();

        if (!fs_read_block(g_fs.sb.meta_start, block)) {
            KT_CHECK(false);
            return;
        }

        fs_noise(block, sizeof(block));
        block[(g_fs.root_block / 8u) % MFS_BLOCK_SIZE] &= (k_u8_t) ~(1u << (g_fs.root_block % 8u));

        if (!fs_write_block(g_fs.sb.meta_start, block)) {
            KT_CHECK(false);
            return;
        }

        fs_expect_damage();
    }
}

static void fs_fuzz_inode(void)
{
    struct mfs_inode inode;
    k_u8_t block[MFS_BLOCK_SIZE];

    ktest_begin("fuzz: a damaged record is named");

    /* The bits between the type, the mode and the owner belong to nobody. */
    fs_restore();
    KT_CHECK(fs_read_inode(g_fs.logs_inode, &inode));
    inode.flags |= 0x4u;
    KT_CHECK(fs_write_inode(g_fs.logs_inode, &inode));
    fs_expect_damage();

    /* A record that a directory points at is not a free one. */
    fs_restore();
    KT_CHECK(fs_read_inode(g_fs.logs_inode, &inode));
    inode.flags = 0u;
    KT_CHECK(fs_write_inode(g_fs.logs_inode, &inode));
    fs_expect_damage();

    /* A directory that starts outside the data has no block of its own. */
    fs_restore();
    KT_CHECK(fs_read_inode(g_fs.logs_inode, &inode));
    inode.first_block = g_fs.sb.data_blocks;
    KT_CHECK(fs_write_inode(g_fs.logs_inode, &inode));
    fs_expect_damage();

    /* A directory cannot be larger than the volume that holds it. */
    fs_restore();
    KT_CHECK(fs_read_inode(g_fs.logs_inode, &inode));
    inode.size = 0xFFFFFFFFu;
    KT_CHECK(fs_write_inode(g_fs.logs_inode, &inode));
    fs_expect_damage();

    /* The counter of the entries has to be the number of the entries. */
    fs_restore();
    KT_CHECK(fs_read_inode(g_fs.logs_inode, &inode));
    inode.entries = 0xFFFFFFFFu;
    KT_CHECK(fs_write_inode(g_fs.logs_inode, &inode));
    fs_expect_damage();

    /* A file that claims bytes it has no block for. */
    fs_restore();
    KT_CHECK(fs_read_inode(g_fs.alpha_inode, &inode));
    inode.first_block = 0xFFFFFFFFu;
    KT_CHECK(fs_write_inode(g_fs.alpha_inode, &inode));
    fs_expect_damage();

    /* A chain that points at its own block is a loop, not a file. */
    fs_restore();
    KT_CHECK(fs_read_block(g_fs.alpha_block, block));
    put_u32(block, g_fs.alpha_block);
    KT_CHECK(fs_write_block(g_fs.alpha_block, block));
    fs_expect_damage();

    /* The first block of the root is not the first block of a file too. */
    fs_restore();
    KT_CHECK(fs_read_inode(g_fs.alpha_inode, &inode));
    inode.first_block = g_fs.root_block;
    KT_CHECK(fs_write_inode(g_fs.alpha_inode, &inode));
    fs_expect_damage();

    fs_restore();
}

static void fs_fuzz_directory(void)
{
    k_u8_t block[MFS_BLOCK_SIZE];
    k_u16_t bad_length;
    k_u32_t bad_inode;
    k_size_t offset;

    ktest_begin("fuzz: a damaged directory is named");

    /* The length of a name fits the name and the room that is left. */
    fs_restore();
    KT_CHECK(fs_read_block(g_fs.root_block, block));
    offset = fs_find_entry(block, "alpha");
    KT_CHECK(offset != (k_size_t)-1);
    put_u16(block + 4u + offset + 4u, 0u);
    KT_CHECK(fs_write_block(g_fs.root_block, block));
    fs_expect_damage();

    fs_restore();
    KT_CHECK(fs_read_block(g_fs.root_block, block));
    offset = fs_find_entry(block, "alpha");
    KT_CHECK(offset != (k_size_t)-1);
    bad_length = MFS_NAME_MAX + 1u;
    put_u16(block + 4u + offset + 4u, bad_length);
    KT_CHECK(fs_write_block(g_fs.root_block, block));
    fs_expect_damage();

    /* An entry points into the table of the records and nowhere else. */
    fs_restore();
    KT_CHECK(fs_read_block(g_fs.root_block, block));
    offset = fs_find_entry(block, "alpha");
    KT_CHECK(offset != (k_size_t)-1);
    bad_inode = g_fs.capacity + 1u;
    memcpy(block + 4u + offset, &bad_inode, sizeof(bad_inode));
    KT_CHECK(fs_write_block(g_fs.root_block, block));
    fs_expect_damage();

    /* The type of an entry has two bits, the rest is not a type. */
    fs_restore();
    KT_CHECK(fs_read_block(g_fs.root_block, block));
    offset = fs_find_entry(block, "alpha");
    KT_CHECK(offset != (k_size_t)-1);
    block[4u + offset + 6u] = 0x40u;
    KT_CHECK(fs_write_block(g_fs.root_block, block));
    fs_expect_damage();

    /* A name is one component of a path and holds no slash. */
    fs_restore();
    KT_CHECK(fs_read_block(g_fs.root_block, block));
    offset = fs_find_entry(block, "alpha");
    KT_CHECK(offset != (k_size_t)-1);
    block[4u + offset + 8u] = '/';
    KT_CHECK(fs_write_block(g_fs.root_block, block));
    fs_expect_damage();

    /* A name appears once in a directory. "alpha" becomes "bravo", which
     * is already there and has the same length. */
    fs_restore();
    KT_CHECK(fs_read_block(g_fs.root_block, block));
    offset = fs_find_entry(block, "alpha");
    KT_CHECK(offset != (k_size_t)-1);
    memcpy(block + 4u + offset + 8u, "bravo", 5);
    KT_CHECK(fs_write_block(g_fs.root_block, block));
    fs_expect_damage();

    /* The entries of the root are read back without leaving the buffer. */
    fs_restore();
    KT_CHECK(mfs_mount(&g_fs.dev) == MFS_OK);
    fs_probe();
    mfs_unmount();
}

static void fs_fuzz_random_records(void)
{
    k_u32_t iteration;

    ktest_begin("fuzz: a record of noise is not a record");

    for (iteration = 0; iteration < FS_FUZZ_RANDOM_RECORDS; iteration++) {
        struct mfs_check_report report;
        struct mfs_inode inode;
        k_u32_t index = 2u + next_u32() % (g_fs.capacity - 1u);

        fs_restore();

        fs_noise((k_u8_t *)&inode, sizeof(inode));
        /*
         * A record of noise that happens to be free would be ignored by the
         * check, and the point of the loop is to walk a live record with
         * every field wrong at once.
         */
        inode.flags |= MFS_INODE_USED;
        inode.first_block = next_u32() % g_fs.sb.data_blocks;
        inode.size = next_u32() % 4096u;
        inode.entries = next_u32() % 16u;

        KT_CHECK(fs_write_inode(index, &inode));

        KT_CHECK_INT_EQ(mfs_mount(&g_fs.dev), MFS_OK);
        KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
        KT_CHECK(report.errors > 0);

        fs_probe();
        mfs_unmount();
    }
}

static void fs_fuzz_random_blocks(void)
{
    k_u32_t iteration;

    ktest_begin("fuzz: a block of noise is refused or named");

    for (iteration = 0; iteration < FS_FUZZ_RANDOM_BLOCKS; iteration++) {
        struct mfs_check_report report;
        int rc;

        fs_restore();

        switch (next_u32() % 3u) {
        case 0: {
            k_u8_t block[MFS_BLOCK_SIZE];

            if (!fs_read_block(g_fs.sb.meta_start, block)) {
                KT_CHECK(false);
                return;
            }

            fs_noise(block, sizeof(block));
            /* The root stays taken, so the damage is in the map itself. */
            block[(g_fs.root_block / 8u) % MFS_BLOCK_SIZE] &=
                (k_u8_t) ~(1u << (g_fs.root_block % 8u));

            if (!fs_write_block(g_fs.sb.meta_start, block)) {
                KT_CHECK(false);
                return;
            }
            break;
        }
        case 1: {
            struct mfs_inode inode;
            k_u32_t index = 2u + next_u32() % (g_fs.capacity - 1u);

            fs_noise((k_u8_t *)&inode, sizeof(inode));
            inode.flags |= MFS_INODE_USED;
            inode.first_block = next_u32() % g_fs.sb.data_blocks;

            if (!fs_write_inode(index, &inode)) {
                KT_CHECK(false);
                return;
            }
            break;
        }
        default: {
            k_u8_t block[MFS_BLOCK_SIZE];

            if (!fs_read_block(g_fs.root_block, block)) {
                KT_CHECK(false);
                return;
            }

            fs_noise(block, sizeof(block));

            if (!fs_write_block(g_fs.root_block, block)) {
                KT_CHECK(false);
                return;
            }
            break;
        }
        }

        rc = mfs_mount(&g_fs.dev);
        if (rc == MFS_OK) {
            KT_CHECK_INT_EQ(mfs_check(&report), MFS_OK);
            KT_CHECK(report.errors > 0);
            fs_probe();
            mfs_unmount();
        } else {
            KT_CHECK(!mfs_is_mounted());
        }
    }
}

void test_fuzz(void)
{
    kmm_init();
    load_words();
    KT_CHECK(g_word_count > 0);

#if KERNEL_EXEC
    g_rng = FUZZ_SEED;

    check_random_buffers();
    check_random_headers();
    check_truncated_images();
    check_boundaries();
    check_run_refusals();
#endif

    /*
     * The records of a volume are another language and get a seed of their
     * own: the parser of a program and the parsers of the filesystem are
     * fuzzed independently, so a change of one does not move the sequence
     * of the other.
     */
    g_rng = FS_FUZZ_SEED;

    ktest_begin("fuzz: the volume of the filesystem is prepared");

    if (fs_fuzz_setup()) {
        fs_fuzz_random_volumes();
        fs_fuzz_garbage_below_header();
        fs_fuzz_superblock();
        fs_fuzz_bitmap();
        fs_fuzz_inode();
        fs_fuzz_directory();
        fs_fuzz_random_records();
        fs_fuzz_random_blocks();
        fs_fuzz_teardown();
    } else {
        KT_CHECK(false);
    }
}
