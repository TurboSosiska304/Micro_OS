/*
 * Micro OS — the allocator of a program.
 *
 * A program has no `brk` and no `mmap`: its image carries a window of data
 * and bss, and the library lends a part of that bss to the allocator. The
 * heap is one static block, and a first-fit list with a header in front of
 * every block is enough for the few allocations a program of this system
 * makes while it runs.
 *
 * The size is a compromise of the window of a program (16 MiB on x86-64)
 * and the rule of the loader that clears at most 1 MiB of bss: 256 KiB is
 * what a regular expression engine and its buffers need, and it leaves the
 * rest of the bss to the program that links the library.
 *
 * There is one task behind the heap, so there is no lock: a program of this
 * system is not a set of threads. A block is aligned for every type the
 * language has, because the compiler may put a `double` in it even though
 * the programs of this tree are built without floating point.
 */
#include "ulibc.h"

#ifndef ULIBC_HEAP_SIZE
#define ULIBC_HEAP_SIZE (256u * 1024u)
#endif

/* The header of a block, rounded up to the strictest alignment. */
struct ulibc_block {
    struct ulibc_block *next;
    size_t size;
    int free;
};

#define ULIBC_BLOCK_HEADER ((sizeof(struct ulibc_block) + 15u) & ~(size_t)15u)
#define ULIBC_ALIGN(size) (((size) + 15u) & ~(size_t)15u)

static union {
    max_align_t align;
    unsigned char bytes[ULIBC_HEAP_SIZE];
} g_heap;

static struct ulibc_block *g_blocks;
static int g_heap_ready;

static void *block_payload(struct ulibc_block *block)
{
    return (unsigned char *)block + ULIBC_BLOCK_HEADER;
}

static struct ulibc_block *payload_block(void *payload)
{
    return (struct ulibc_block *)((unsigned char *)payload - ULIBC_BLOCK_HEADER);
}

static void heap_init(void)
{
    g_blocks = (struct ulibc_block *)g_heap.bytes;
    g_blocks->next = NULL;
    g_blocks->size = ULIBC_HEAP_SIZE - ULIBC_BLOCK_HEADER;
    g_blocks->free = 1;
    g_heap_ready = 1;
}

void *malloc(size_t size)
{
    struct ulibc_block *block;

    if (!g_heap_ready) {
        heap_init();
    }

    if (size > (size_t)-1 - 15u) {
        return NULL;
    }

    size = ULIBC_ALIGN(size);
    if (size == 0) {
        size = 16;
    }

    for (block = g_blocks; block != NULL; block = block->next) {
        if (!block->free || block->size < size) {
            continue;
        }

        /* A remainder that is too small to hold a header and a useful
         * payload stays with the block; the caller may use it. */
        if (block->size >= size + ULIBC_BLOCK_HEADER + 16u) {
            struct ulibc_block *rest =
                (struct ulibc_block *)((unsigned char *)block_payload(block) + size);

            rest->size = block->size - size - ULIBC_BLOCK_HEADER;
            rest->free = 1;
            rest->next = block->next;
            block->next = rest;
            block->size = size;
        }

        block->free = 0;

        return block_payload(block);
    }

    return NULL;
}

void free(void *memory)
{
    struct ulibc_block *block;
    struct ulibc_block *previous = NULL;

    if (memory == NULL) {
        return;
    }

    block = payload_block(memory);
    block->free = 1;

    /* The list is in the order of the heap, so one walk finds the block
     * before this one and every neighbour that can be joined. */
    for (struct ulibc_block *walk = g_blocks; walk != NULL && walk != block; walk = walk->next) {
        previous = walk;
    }

    if (block->next != NULL && block->next->free) {
        block->size += ULIBC_BLOCK_HEADER + block->next->size;
        block->next = block->next->next;
    }

    if (previous != NULL && previous->free) {
        previous->size += ULIBC_BLOCK_HEADER + block->size;
        previous->next = block->next;
    }
}

void *calloc(size_t count, size_t size)
{
    size_t total;
    void *memory;

    if (count != 0 && size > (size_t)-1 / count) {
        return NULL;
    }

    total = count * size;
    memory = malloc(total);

    if (memory != NULL) {
        memset(memory, 0, total);
    }

    return memory;
}

void *realloc(void *memory, size_t size)
{
    struct ulibc_block *block;
    void *grown;

    if (memory == NULL) {
        return malloc(size);
    }

    if (size == 0) {
        free(memory);
        return NULL;
    }

    block = payload_block(memory);
    if (block->size >= size) {
        return memory;
    }

    grown = malloc(size);
    if (grown == NULL) {
        return NULL;
    }

    memcpy(grown, memory, block->size);
    free(memory);

    return grown;
}
