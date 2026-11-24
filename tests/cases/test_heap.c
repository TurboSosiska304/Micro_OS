/*
 * Micro OS — the tests of the allocator of a program.
 *
 * `components/libc/user/src/malloc.c` cannot be linked into the tests of the
 * kernel (it is the library of a program, and it would answer the `malloc` of
 * the C library of the host), so it has a test binary of its own: the file is
 * compiled for the host with the door of `tests/cases/userland_door_shim.c`
 * behind it, and that door keeps a heap of its own for the call 47, `brk`.
 *
 * The properties: a block comes back with the bytes of the caller written in
 * it, a block that was freed is used again before the heap grows, the heap
 * grows past the arena of the bss (`ULIBC_HEAP_SIZE`) when the program asks
 * for more than it, a refusal of `brk` is ENOMEM with the heap it had, and no
 * small allocation after the first growth costs a call of the machine.
 */
#include "ktest.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

void test_heap(void);

/* The heap of the door (tests/cases/userland_door_shim.c). */
void userland_door_brk_limit(unsigned long limit);
unsigned long userland_door_brk_calls(void);
void userland_door_brk_reset(void);

#define BLOCK_COUNT 512
#define BLOCK_SIZE 64

static unsigned char *g_blocks[BLOCK_COUNT];

static void test_alignment(void)
{
    void *small = malloc(1);
    void *medium = malloc(4096);
    void *large = malloc(1024 * 1024 + 17);

    KT_CHECK(small != NULL);
    KT_CHECK(medium != NULL);
    KT_CHECK(large != NULL);

    KT_CHECK(((uintptr_t)small % 16u) == 0);
    KT_CHECK(((uintptr_t)medium % 16u) == 0);
    KT_CHECK(((uintptr_t)large % 16u) == 0);

    free(small);
    free(medium);
    free(large);
}

/* A block of the caller is a block of the caller and nothing of somebody
 * else: every byte of every live block keeps what was written in it. */
static void test_blocks_are_separate(void)
{
    k_size_t i;

    memset(g_blocks, 0, sizeof(g_blocks));

    for (i = 0; i < BLOCK_COUNT; i++) {
        g_blocks[i] = (unsigned char *)malloc(BLOCK_SIZE);
        KT_CHECK(g_blocks[i] != NULL);
        if (g_blocks[i] == NULL) {
            return;
        }
        memset(g_blocks[i], (int)(i & 0xFFu), BLOCK_SIZE);
    }

    for (i = 0; i < BLOCK_COUNT; i++) {
        k_size_t j;

        for (j = 0; j < BLOCK_SIZE; j++) {
            if (g_blocks[i][j] != (unsigned char)(i & 0xFFu)) {
                KT_CHECK(0);
                return;
            }
        }
    }

    /* Every other block goes back: the heap has holes and the next round of
     * allocations lives in them without touching the blocks that stayed. */
    for (i = 0; i < BLOCK_COUNT; i += 2) {
        free(g_blocks[i]);
        g_blocks[i] = NULL;
    }

    for (i = 0; i < BLOCK_COUNT; i += 2) {
        g_blocks[i] = (unsigned char *)malloc(BLOCK_SIZE);
        KT_CHECK(g_blocks[i] != NULL);
        if (g_blocks[i] != NULL) {
            memset(g_blocks[i], 0x5A, BLOCK_SIZE);
        }
    }

    for (i = 1; i < BLOCK_COUNT; i += 2) {
        k_size_t j;

        for (j = 0; j < BLOCK_SIZE; j++) {
            if (g_blocks[i][j] != (unsigned char)(i & 0xFFu)) {
                KT_CHECK(0);
                return;
            }
        }
    }

    for (i = 0; i < BLOCK_COUNT; i++) {
        free(g_blocks[i]);
        g_blocks[i] = NULL;
    }
}

/* A freed block is a block of the heap that the next allocation of the same
 * size gets back, and the heap does not grow for it. */
static void test_free_is_reused(void)
{
    unsigned long calls = userland_door_brk_calls();
    void *block = malloc(200);
    void *again;

    KT_CHECK(block != NULL);
    free(block);
    again = malloc(200);
    KT_CHECK(again == block);

    KT_CHECK_INT_EQ((int)(userland_door_brk_calls() - calls), 0);
}

/*
 * The refusal of the machine. The door stops the heap where it is, and the
 * allocation of a block that does not fit answers NULL with ENOMEM and leaves
 * the rest of the heap usable - a `malloc` that had already spent half of the
 * arena would fail the next small block of a command.
 */
static void test_refusal_is_enomem(void)
{
    void *big;
    void *small;

    /* No growth at all: the arena of the bss is 256 KiB, so a block of
     * 512 KiB cannot come from anywhere but the machine. */
    userland_door_brk_limit(0);

    big = malloc(512 * 1024);
    KT_CHECK(big == NULL);
    KT_CHECK_INT_EQ(errno, ENOMEM);

    small = malloc(32);
    KT_CHECK(small != NULL);
    free(small);

    userland_door_brk_reset();

    big = malloc(512 * 1024);
    KT_CHECK(big != NULL);
    free(big);
}

/* More than the arena, and more than the budget of a task of x86-64: the
 * heap asks the kernel for the memory and the program gets it. */
static void test_grows_past_the_arena(void)
{
    const size_t size = 3u * 1024u * 1024u;
    unsigned char *block;
    unsigned char *again;
    unsigned long calls;

    calls = userland_door_brk_calls();

    block = (unsigned char *)malloc(size);
    KT_CHECK(block != NULL);
    if (block == NULL) {
        return;
    }

    block[0] = 0x11;
    block[size / 2] = 0x22;
    block[size - 1] = 0x33;

    KT_CHECK(block[0] == 0x11);
    KT_CHECK(block[size / 2] == 0x22);
    KT_CHECK(block[size - 1] == 0x33);

    free(block);

    /* The memory stayed with the heap of the program: a second block of the
     * same size comes from it and not from a second call of `brk`. */
    KT_CHECK(userland_door_brk_calls() > calls);

    calls = userland_door_brk_calls();
    again = (unsigned char *)malloc(size);
    KT_CHECK(again != NULL);
    KT_CHECK_INT_EQ((int)(userland_door_brk_calls() - calls), 0);
    if (again != NULL) {
        again[size - 1] = 0x44;
        KT_CHECK(again[size - 1] == 0x44);
    }
    free(again);
}

static void test_calloc_and_realloc(void)
{
    unsigned char *block = (unsigned char *)calloc(1024, 8);
    k_size_t i;
    unsigned char *grown;
    int zeroed = 1;

    KT_CHECK(block != NULL);
    if (block == NULL) {
        return;
    }

    for (i = 0; i < 1024 * 8; i++) {
        if (block[i] != 0) {
            zeroed = 0;
            break;
        }
    }
    KT_CHECK(zeroed);

    for (i = 0; i < 1024 * 8; i++) {
        block[i] = (unsigned char)(i & 0xFFu);
    }

    grown = (unsigned char *)realloc(block, 64 * 1024);
    KT_CHECK(grown != NULL);
    if (grown == NULL) {
        free(block);
        return;
    }

    for (i = 0; i < 1024 * 8; i++) {
        if (grown[i] != (unsigned char)(i & 0xFFu)) {
            KT_CHECK(0);
            break;
        }
    }

    /* A realloc to zero frees the block and answers NULL, as C says. */
    KT_CHECK(realloc(grown, 0) == NULL);

    /* A realloc of a small block to a smaller size keeps it where it is. */
    block = (unsigned char *)malloc(4096);
    KT_CHECK(block != NULL);
    if (block != NULL) {
        unsigned char *same = (unsigned char *)realloc(block, 128);

        KT_CHECK(same == block);
        free(same);
    }
}

static void test_the_edges(void)
{
    static volatile size_t g_wrap = (size_t)-1;
    void *nothing;

    KT_CHECK(malloc(0) == NULL);
    free(NULL);

    /* A count of zero is no memory and not a block of no bytes. */
    nothing = calloc(0, 16);
    KT_CHECK(nothing == NULL);
    free(nothing);

    /* The count of the bytes wraps: the answer is ENOMEM and not a small
     * block that the caller would write past. */
    errno = 0;
    KT_CHECK(calloc(g_wrap, 2) == NULL);
    KT_CHECK_INT_EQ(errno, ENOMEM);
}

void test_heap(void)
{
    /* The refusal first: a heap that already holds a large freed block would
     * answer it from that block and not from the machine. */
    test_the_edges();
    test_refusal_is_enomem();
    test_alignment();
    test_blocks_are_separate();
    test_free_is_reused();
    test_grows_past_the_arena();
    test_calloc_and_realloc();
}
