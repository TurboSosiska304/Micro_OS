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

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "kernel/config.h"
#include "kernel/errno.h"
#include "kernel/exec.h"
#include "kernel/ksys.h"
#include "kernel/port/exec.h"

#if KERNEL_EXEC

#ifndef KTEST_MOX_FIXTURE
#define KTEST_MOX_FIXTURE "tests/corpus/mox1/frozen-x86_64.hex"
#endif

#define TEST_TEXT_BYTES 8u
#define TEST_DATA_BYTES 4u
#define TEST_IMAGE_SIZE (K_EXEC_HEADER_SIZE + TEST_TEXT_BYTES + TEST_DATA_BYTES)

/*
 * The image of 1.0, frozen in the corpus (tests/corpus/mox1): its size, its
 * sum, and the fields of the header as the file records them. The numbers are
 * a second lock on the bytes: a changed byte of the fixture fails the sum and
 * not only the parse.
 */
#define FROZEN_SIZE 4128u
#define FROZEN_SUM 0x94256496u
#define FROZEN_ENTRY K_EXEC_HEADER_SIZE
#define FROZEN_TEXT_SIZE 4064u
#define FROZEN_DATA_SIZE 32u
#define FROZEN_BSS_SIZE 0u

static k_u8_t g_frozen[8192];
static k_size_t g_frozen_size;

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    return c - 'A' + 10;
}

/*
 * Read the fixture: hex, two digits to a byte, `#` starts a comment. The file
 * is text on purpose - a diff of the record says which byte of the image
 * changed, which a binary blob of the tree would not.
 */
static int load_frozen(void)
{
    FILE *file = fopen(KTEST_MOX_FIXTURE, "r");
    char line[256];
    k_size_t used = 0;

    if (file == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *comment = strchr(line, '#');
        const char *cursor = line;

        if (comment != NULL) {
            *comment = '\0';
        }

        for (;;) {
            while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
                cursor++;
            }

            if (*cursor == '\0') {
                break;
            }

            if (!isxdigit((unsigned char)cursor[0]) || !isxdigit((unsigned char)cursor[1]) ||
                used >= sizeof(g_frozen)) {
                fclose(file);
                return -1;
            }

            g_frozen[used++] = (k_u8_t)((hex_digit(cursor[0]) << 4) | hex_digit(cursor[1]));
            cursor += 2;
        }
    }

    fclose(file);
    g_frozen_size = used;

    return (used > 0) ? 0 : -1;
}

/* The sum of the bytes, the one the record of the fixture carries. */
static k_u32_t frozen_sum(const k_u8_t *data, k_size_t size)
{
    k_u32_t sum = 2166136261u;
    k_size_t i;

    for (i = 0; i < size; i++) {
        sum ^= data[i];
        sum *= 16777619u;
    }

    return sum;
}

/* A field of the header, little endian, read the way the parser reads it. */
static k_u32_t frozen_word(k_size_t at)
{
    return (k_u32_t)g_frozen[at] | ((k_u32_t)g_frozen[at + 1] << 8) |
           ((k_u32_t)g_frozen[at + 2] << 16) | ((k_u32_t)g_frozen[at + 3] << 24);
}

static void put_u32(k_u8_t *at, k_u32_t value)
{
    at[0] = (k_u8_t)(value & 0xFFu);
    at[1] = (k_u8_t)((value >> 8) & 0xFFu);
    at[2] = (k_u8_t)((value >> 16) & 0xFFu);
    at[3] = (k_u8_t)((value >> 24) & 0xFFu);
}

/*
 * A consistent image: header, eight bytes of code, four of data. The machine
 * is a parameter: a file of the board is as well formed as a file of the
 * simulator, and only the kernel that runs it says which one it accepts.
 */
static k_size_t build_image_arch(k_u8_t *image, k_u32_t arch)
{
    memset(image, 0, TEST_IMAGE_SIZE);

    put_u32(&image[0], K_EXEC_MAGIC);
    put_u32(&image[4], K_EXEC_VERSION);
    put_u32(&image[8], 0); /* flags */
    put_u32(&image[12], K_EXEC_HEADER_SIZE);
    put_u32(&image[16], TEST_TEXT_BYTES);
    put_u32(&image[20], TEST_DATA_BYTES);
    put_u32(&image[24], 0);    /* bss */
    put_u32(&image[28], arch); /* the machine of the bytes */

    /* Something that looks like code and data; the parser does not run it. */
    image[K_EXEC_HEADER_SIZE + 0] = 0xC3; /* ret */
    image[K_EXEC_HEADER_SIZE + 1] = 0x90; /* nop */
    image[K_EXEC_HEADER_SIZE + TEST_TEXT_BYTES + 0] = 0x2A;

    return TEST_IMAGE_SIZE;
}

static k_size_t build_image(k_u8_t *image)
{
    return build_image_arch(image, K_EXEC_ARCH_X86_64);
}

/*
 * The two limits of a call: an image has to hold the header, and it may not
 * pass K_EXEC_MAX_IMAGE (8 MiB), which is the memory of the machine and not
 * the transfer edge of a call - an image at the boundary is read in pieces of
 * KSYS_MAX_IO by the loop of sys_execve.
 */
static void check_size_boundary(void)
{
    ktest_begin("exec: an image at the 8 MiB limit is read, one past it is E2BIG");

    KT_CHECK_INT_EQ(K_EXEC_MAX_IMAGE, 8u * 1024u * 1024u);
    KT_CHECK_INT_EQ(k_exec_image_check(0), KERR_INVAL);
    KT_CHECK_INT_EQ(k_exec_image_check(K_EXEC_HEADER_SIZE - 1u), KERR_INVAL);
    KT_CHECK_INT_EQ(k_exec_image_check(K_EXEC_HEADER_SIZE), 0);
    KT_CHECK_INT_EQ(k_exec_image_check(K_EXEC_MAX_IMAGE - 1u), 0);
    KT_CHECK_INT_EQ(k_exec_image_check(K_EXEC_MAX_IMAGE), 0);
    KT_CHECK_INT_EQ(k_exec_image_check(K_EXEC_MAX_IMAGE + 1u), KERR_TOOBIG);
    /* The transfer edge of one call is smaller than the image and is not
     * what this check names. */
    KT_CHECK(K_EXEC_MAX_IMAGE > KSYS_MAX_IO);
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
    KT_CHECK_INT_EQ(parsed.arch, K_EXEC_ARCH_X86_64);
    KT_CHECK_INT_EQ(parsed.entry, K_EXEC_HEADER_SIZE);
    KT_CHECK_INT_EQ(parsed.text_size, TEST_TEXT_BYTES);
    KT_CHECK_INT_EQ(parsed.data_size, TEST_DATA_BYTES);
    KT_CHECK_INT_EQ(parsed.bss_size, 0);
    /* The file carries no environment; the caller of the run sets one. */
    KT_CHECK(parsed.envp == NULL);
}

/*
 * The environment of the system is the one every program started without a
 * vector of its own sees. The property that matters is the shape of the
 * vector and not the words: PATH is a value a program of this system looks
 * for, and no word may be a name without a value.
 */
static void check_environment(void)
{
    const char *const *envp = k_exec_environ();
    k_size_t count = 0;

    ktest_begin("exec: the environment of the system is a vector of names");

    KT_CHECK(envp != NULL);
    KT_CHECK(k_exec_environ() == envp);

    while (envp != NULL && envp[count] != NULL && count < 32u) {
        const char *word = envp[count];
        const char *equals = strchr(word, '=');

        KT_CHECK(equals != NULL);
        KT_CHECK(equals != word);
        KT_CHECK(equals == NULL || equals[1] != '\0');
        count++;
    }

    KT_CHECK(count > 0);
    KT_CHECK(count < 32u);
    KT_CHECK(envp != NULL && envp[count] == NULL);
    KT_CHECK(envp != NULL && strcmp(envp[0], "PATH=/bin") == 0);
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

    /* The version is the one this kernel reads: version 1 kept the machine
     * of the image implicit, and a later generation is a contract this
     * reader does not know. Either is a refusal, not a header to guess at. */
    size = build_image(image);
    put_u32(&image[4], 1u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[4], K_EXEC_VERSION + 1u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    /* The one flag the format names is the request of a page both writable
     * and executable, and no port of this system builds one: the parser
     * refuses it with its own sentence. Any other bit is a generation this
     * kernel does not know either. */
    size = build_image(image);
    put_u32(&image[8], K_EXEC_FLAG_WX);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image(image);
    put_u32(&image[8], 0x80000000u);
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

/*
 * The machine is part of the header and the parser carries it: a file of the
 * board is an image of this format exactly as a file of x86-64 is, and the
 * machine that runs it is what decides. A number the format does not know is
 * not an image at all - nothing says what its bytes mean.
 */
static void check_arch(void)
{
    k_u8_t image[TEST_IMAGE_SIZE];
    struct k_exec_image parsed;
    k_size_t size;

    ktest_begin("exec: the machine of an image is read, an unknown one refused");

    memset(&parsed, 0, sizeof(parsed));
    size = build_image_arch(image, K_EXEC_ARCH_XTENSA);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), 0);
    KT_CHECK_INT_EQ(parsed.arch, K_EXEC_ARCH_XTENSA);

    size = build_image_arch(image, K_EXEC_ARCH_X86_64);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), 0);
    KT_CHECK_INT_EQ(parsed.arch, K_EXEC_ARCH_X86_64);

    size = build_image_arch(image, 2u);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);

    size = build_image_arch(image, 0xFFFFFFFFu);
    KT_CHECK_INT_EQ(k_exec_parse(image, size, &parsed), -1);
}

/*
 * The image of 1.0 (tests/corpus/mox1/frozen-x86_64.hex): a program built for
 * the format of this release and recorded in the corpus. The property is the
 * promise of a release: an image that was built for 1.0 is read by every 1.x.
 * The parser is asked for the fields it would hand to the port, and the bytes
 * are checked against the sum of the record, so neither the parser nor the
 * fixture can move without the failure that says so.
 *
 * The machine is the one of the file, not the one of the test: the host is a
 * machine that cannot enter a program, and the parse of the image of x86-64 is
 * still the parse that a machine of that port would do.
 */
static void check_frozen_image(void)
{
    struct k_exec_image parsed;

    ktest_begin("exec: the image of 1.0 is read as it was frozen");

    if (load_frozen() != 0) {
        KT_CHECK(0);
        return;
    }

    KT_CHECK_INT_EQ(g_frozen_size, FROZEN_SIZE);
    KT_CHECK_INT_EQ(frozen_sum(g_frozen, g_frozen_size), FROZEN_SUM);

    /* The header of the record, word by word. */
    KT_CHECK_INT_EQ(frozen_word(0), K_EXEC_MAGIC);
    KT_CHECK_INT_EQ(frozen_word(4), K_EXEC_VERSION);
    KT_CHECK_INT_EQ(frozen_word(8), 0); /* flags: none of the format */
    KT_CHECK_INT_EQ(frozen_word(12), K_EXEC_HEADER_SIZE);
    KT_CHECK_INT_EQ(frozen_word(16), FROZEN_TEXT_SIZE);
    KT_CHECK_INT_EQ(frozen_word(20), FROZEN_DATA_SIZE);
    KT_CHECK_INT_EQ(frozen_word(24), FROZEN_BSS_SIZE);
    KT_CHECK_INT_EQ(frozen_word(28), K_EXEC_ARCH_X86_64);

    /* And what the parser makes of it: the fields the port is handed. */
    memset(&parsed, 0, sizeof(parsed));
    KT_CHECK_INT_EQ(k_exec_parse(g_frozen, g_frozen_size, &parsed), 0);
    KT_CHECK(parsed.data == g_frozen);
    KT_CHECK_INT_EQ(parsed.size, g_frozen_size);
    KT_CHECK_INT_EQ(parsed.arch, K_EXEC_ARCH_X86_64);
    KT_CHECK_INT_EQ(parsed.entry, FROZEN_ENTRY);
    KT_CHECK_INT_EQ(parsed.text_size, FROZEN_TEXT_SIZE);
    KT_CHECK_INT_EQ(parsed.data_size, FROZEN_DATA_SIZE);
    KT_CHECK_INT_EQ(parsed.bss_size, FROZEN_BSS_SIZE);
    KT_CHECK(parsed.envp == NULL);

    /* The rule of the port against the windows of protection: the text ends
     * on a page of its own, so a machine can give it the one protection of
     * code and the data the one of memory (NX/W^X, ADR 0007). The fixture has
     * to be an image a port of 1.0 would accept, not only one the parser
     * reads. */
    KT_CHECK_INT_EQ((K_EXEC_HEADER_SIZE + parsed.text_size) % 4096u, 0);
    KT_CHECK(parsed.entry >= K_EXEC_HEADER_SIZE);
    KT_CHECK(parsed.entry < parsed.text_size);
    KT_CHECK(parsed.size >= K_EXEC_HEADER_SIZE + parsed.text_size + parsed.data_size);
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
    KT_CHECK_INT_EQ(parsed.arch, before.arch);
    KT_CHECK_INT_EQ(parsed.entry, before.entry);
    KT_CHECK_INT_EQ(parsed.text_size, before.text_size);
    KT_CHECK_INT_EQ(parsed.data_size, before.data_size);
    KT_CHECK_INT_EQ(parsed.bss_size, before.bss_size);

    /*
     * The machine of the image against the machine of the port. A port that
     * names itself refuses the number of the other machine before it is
     * asked whether it can run anything; a port with no machine to name (-1)
     * has nothing to compare and keeps the answer of "this machine cannot
     * run a program". The number the port gives is one of the three, never a
     * machine of the header that this kernel does not know.
     */
    {
        const int here = k_port_exec_arch();
        struct k_exec_image foreign;

        KT_CHECK(here == -1 || here == (int)K_EXEC_ARCH_X86_64 || here == (int)K_EXEC_ARCH_XTENSA);

        size = build_image_arch(image, (here == (int)K_EXEC_ARCH_XTENSA) ? K_EXEC_ARCH_X86_64
                                                                         : K_EXEC_ARCH_XTENSA);
        memset(&foreign, 0, sizeof(foreign));
        KT_CHECK_INT_EQ(k_exec_parse(image, size, &foreign), 0);
        KT_CHECK_INT_EQ(k_exec_run(&foreign, NULL),
                        (here >= 0) ? K_EXEC_RUN_OTHER_ARCH : K_EXEC_RUN_NO_MACHINE);
    }

    /*
     * The status of a program that ran somewhere else is not the answer of a
     * machine that refuses this one. The refusal comes first, and a caller
     * that would read the number instead of the refusal would read the
     * artifact of an earlier run.
     */
    k_exec_exit(7);
    KT_CHECK_INT_EQ(k_exec_run(&parsed, NULL), -2);

    /*
     * An environment of the caller is carried by the image and not by the
     * machine: a machine that cannot enter a program says so whatever the
     * vector is, and the structure of the caller is not written to - the
     * default of the system is a copy of the call, not a field of the image.
     */
    {
        char *env[] = {(char *)"PATH=/bin", (char *)"HOME=/", NULL};
        struct k_exec_image with_env = parsed;

        with_env.envp = (const char *const *)env;
        KT_CHECK_INT_EQ(k_exec_run(&with_env, NULL), -2);
        KT_CHECK(with_env.envp != NULL);
        KT_CHECK(with_env.envp[0] == env[0]);
    }

    /* A call with nothing to run is a refusal of the caller, not of the
     * machine, so it is -1 and not ENOSYS. */
    KT_CHECK_INT_EQ(k_exec_run(NULL, NULL), -1);

    /* This machine has no address space per task: a fork cannot copy the
     * context of the caller, and the kernel has to keep its placeholder. */
    KT_CHECK_INT_EQ(k_port_fork_copy(NULL), -1);
}

void test_exec(void)
{
    check_size_boundary();
    check_valid_image();
    check_bad_header();
    check_bad_sizes();
    check_bad_entry();
    check_arch();
    check_frozen_image();
    check_environment();
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
