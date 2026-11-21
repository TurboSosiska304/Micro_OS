/*
 * Micro OS — the format of a program and the contract that runs it.
 *
 * The parser is tested with images made byte by byte in the test, so that a
 * change of the layout is visible here and not in a file that happened to be
 * built with the same mistake. The machine that runs the tests (the host) is
 * one that cannot enter a program; that is a fact worth a test of its own,
 * because it is what makes execve say ENOSYS in the simulator.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/config.h"
#include "kernel/exec.h"
#include "kernel/port/exec.h"

#if KERNEL_EXEC

#define TEST_TEXT_BYTES 8u
#define TEST_DATA_BYTES 4u
#define TEST_IMAGE_SIZE (K_EXEC_HEADER_SIZE + TEST_TEXT_BYTES + TEST_DATA_BYTES)

static void put_u32(k_u8_t *at, k_u32_t value)
{
    at[0] = (k_u8_t)(value & 0xFFu);
    at[1] = (k_u8_t)((value >> 8) & 0xFFu);
    at[2] = (k_u8_t)((value >> 16) & 0xFFu);
    at[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/* A consistent image: header, eight bytes of code, four of data. */
static k_size_t build_image(k_u8_t *image)
{
    memset(image, 0, TEST_IMAGE_SIZE);

    put_u32(&image[0], K_EXEC_MAGIC);
    put_u32(&image[4], K_EXEC_VERSION);
    put_u32(&image[8], 0); /* flags */
    put_u32(&image[12], K_EXEC_HEADER_SIZE);
    put_u32(&image[16], TEST_TEXT_BYTES);
    put_u32(&image[20], TEST_DATA_BYTES);
    put_u32(&image[24], 0); /* bss */
    put_u32(&image[28], 0); /* reserved */

    /* Something that looks like code and data; the parser does not run it. */
    image[K_EXEC_HEADER_SIZE + 0] = 0xC3; /* ret */
    image[K_EXEC_HEADER_SIZE + 1] = 0x90; /* nop */
    image[K_EXEC_HEADER_SIZE + TEST_TEXT_BYTES + 0] = 0x2A;

    return TEST_IMAGE_SIZE;
}

static void check_valid_image(void)
{
    k_u8_t image[TEST_IMAGE_SIZE];
    struct k_exec_image parsed;
    k_size_t size = build_image(image);

    ktest_begin("exec: a consistent image is accepted");

    memset(&parsed, 0, sizeof(parsed));
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), 0);

    KT_CHECK(parsed.data == image);
    KT_CHECK_INT_EQ(parsed.size, size);
    KT_CHECK_INT_EQ(parsed.entry, K_EXEC_HEADER_SIZE);
    KT_CHECK_INT_EQ(parsed.text_size, TEST_TEXT_BYTES);
    KT_CHECK_INT_EQ(parsed.data_size, TEST_DATA_BYTES);
    KT_CHECK_INT_EQ(parsed.bss_size, 0);
}

static void check_bad_header(void)
{
    k_u8_t image[TEST_IMAGE_SIZE];
    struct k_exec_image parsed;
    k_size_t size;

    ktest_begin("exec: a bad header is refused");

    size = build_image(image);
    image[0] ^= 0xFFu;
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[4], K_EXEC_VERSION + 1u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    /* An unknown flag and a reserved word that is not zero are generations
     * this kernel does not know, not a header to guess about. */
    size = build_image(image);
    put_u32(&image[8], 1u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[28], 1u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    /* A file shorter than the header is not an image at all. */
    size = build_image(image);
    KT_CHECK_INT_EQ(k_exec_parse(image, K_EXEC_HEADER_SIZE - 1u, &parsed), -1);

    KT_CHECK_INT_EQ(k_exec_parse(NULL, size, &parsed), -1);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, NULL), -1);
}

static void check_bad_sizes(void)
{
    k_u8_t image[TEST_IMAGE_SIZE];
    struct k_exec_image parsed;
    k_size_t size;

    ktest_begin("exec: sizes that do not fit are refused");

    /* The segments claim more than the file holds. */
    size = build_image(image);
    put_u32(&image[16], TEST_TEXT_BYTES + 4u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    /* The segments run past the end of a file that is too short. */
    size = build_image(image);
    KT_CHECK_INT_EQ(k_exec_parse(image, size - 4u, &parsed), -1);

    /* The port copies and clears in words, so an odd size would tear. */
    size = build_image(image);
    put_u32(&image[16], TEST_TEXT_BYTES - 1u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[20], TEST_DATA_BYTES - 3u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[24], 2u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    /* No text, no entry. */
    size = build_image(image);
    put_u32(&image[16], 0);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    /* A sum that would pass the end of a 32-bit address space is refused by
     * the 64-bit arithmetic, not by a wrapped comparison. */
    size = build_image(image);
    put_u32(&image[16], 0xFFFFFFFCu);
    put_u32(&image[20], 0xFFFFFFFCu);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    /* The zeroed part is not in the file, so the parser puts a ceiling on it. */
    size = build_image(image);
    put_u32(&image[24], K_EXEC_BSS_MAX + 4u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[24], K_EXEC_BSS_MAX);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), 0);
}

static void check_bad_entry(void)
{
    k_u8_t image[TEST_IMAGE_SIZE];
    struct k_exec_image parsed;
    k_size_t size;

    ktest_begin("exec: the entry has to point into the text");

    /* The header is not code and the data may not be executed. */
    size = build_image(image);
    put_u32(&image[12], 0);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[12], K_EXEC_HEADER_SIZE + TEST_TEXT_BYTES);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[12], (k_u32_t)size);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    /* A byte inside the text is fine, the instruction boundary is the job of
     * the compiler that made the image. */
    size = build_image(image);
    put_u32(&image[12], K_EXEC_HEADER_SIZE + 1u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), 0);
}

static void check_run_without_a_machine(void)
{
    k_u8_t image[TEST_IMAGE_SIZE];
    k_u8_t copy[TEST_IMAGE_SIZE];
    struct k_exec_image parsed;
    struct k_exec_image before;
    k_size_t size = build_image(image);

    ktest_begin("exec: the host cannot enter a program");

    KT_CHECK(!k_port_exec_supported());
    KT_CHECK(k_port_exec_base() == NULL);

    /*
     * The seam of copy_from_user on a machine without an address space per
     * program: a pointer of a call is memory of the kernel already, so the
     * range at the edge is accepted and the empty range is always accepted.
     */
    KT_CHECK(k_port_exec_contains(image, size));
    KT_CHECK(k_port_exec_contains(NULL, 0));

    /* No program of this machine is ever in flight, so an exit is a no-op and
     * the kernel's exit of a task stays the path of the board and the host. */
    KT_CHECK(!k_port_exec_active());
    k_port_exec_exit(3);
    KT_CHECK(!k_port_exec_active());

    memset(&parsed, 0, sizeof(parsed));
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), 0);

    memcpy(copy, image, sizeof(copy));
    before = parsed;

    KT_CHECK_INT_EQ(k_exec_run(&parsed, NULL), -2);

    /* -2 is "this machine cannot", not a reason to touch the image. */
    KT_CHECK(memcmp(copy, image, sizeof(copy)) == 0);
    KT_CHECK(parsed.data == before.data);
    KT_CHECK_INT_EQ(parsed.size, before.size);
    KT_CHECK_INT_EQ(parsed.entry, before.entry);
    KT_CHECK_INT_EQ(parsed.text_size, before.text_size);
    KT_CHECK_INT_EQ(parsed.data_size, before.data_size);
    KT_CHECK_INT_EQ(parsed.bss_size, before.bss_size);

    /*
     * The status of a program that ran somewhere else is not the answer of a
     * machine that refuses this one. The refusal comes first, and a caller
     * that would read the number instead of the refusal would read the
     * artifact of an earlier run.
     */
    k_exec_exit(7);
    KT_CHECK_INT_EQ(k_exec_run(&parsed, NULL), -2);

    /* A call with nothing to run is a refusal of the caller, not of the
     * machine, so it is -1 and not ENOSYS. */
    KT_CHECK_INT_EQ(k_exec_run(NULL, NULL), -1);

    /* This machine has no address space per task: a fork cannot copy the
     * context of the caller, and the kernel has to keep its placeholder. */
    KT_CHECK_INT_EQ(k_port_fork_copy(NULL), -1);
}

void test_exec(void)
{
    check_valid_image();
    check_bad_header();
    check_bad_sizes();
    check_bad_entry();
    check_run_without_a_machine();
}

#else /* !KERNEL_EXEC */

/*
 * A build without the parser has nothing to test here, and the test says so
 * instead of disappearing: a reader of the report knows which build ran.
 */
void test_exec(void)
{
    ktest_begin("exec: the MOX1 parser is not in this build (KERNEL_EXEC=0)");
    KT_CHECK_INT_EQ(KERNEL_EXEC, 0);
}

#endif /* KERNEL_EXEC */
