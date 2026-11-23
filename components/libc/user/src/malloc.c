/*
 * Micro OS — the allocator of a program.
 *
 * The kernel has `KSYS_MALLOC`, and a program cannot use it: the block comes
 * from the heap of ring 0, which the page tables of the program map without
 * the user bit. The arena therefore lives where the program itself lives - a
 * static array of its bss - and the calls stay inside the window the port
 * mapped writable.
 *
 * The shape is a free list of blocks with a boundary tag and no header of the
 * caller: every block starts with its size, a flag and the two links of the
 * list. A block is split when the rest can hold a header and the smallest
 * useful payload, and neighbours are joined when a block is freed, so a loop
 * of allocate-free does not walk the arena down. It is the simplest allocator
 * that a ported library with a few hundred small blocks can live in.
 *
 * The size is a compile-time setting because the bss of an image is capped at
 * a megabyte by the format and a program of the board has no room for it at
 * all; a program that allocates is built with `-DULIBC_HEAP_SIZE=...`.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdint.h>

#ifndef ULIBC_HEAP_SIZE
#define ULIBC_HEAP_SIZE (256u * 1024u)
#endif

#define HEAP_ALIGN 16u
#define HEAP_MIN_PAYLOAD 16u

struct heap_block {
    size_t size; /* the block itself, its header included */
    int free;
    struct heap_block *next;
    struct heap_block *previous;
};

_Alignas(16) static unsigned char g_heap[ULIBC_HEAP_SIZE];
static struct heap_block *g_heap_free;
static int g_heap_ready;

static size_t heap_align(size_t size)
{
    return (size + (HEAP_ALIGN - 1u)) & ~(size_t)(HEAP_ALIGN - 1u);
}

static void heap_init(void)
{
    struct heap_block *block = (struct heap_block *)g_heap;

    block->size = sizeof(g_heap);
    block->free = 1;
    block->next = NULL;
    block->previous = NULL;

    g_heap_free = block;
    g_heap_ready = 1;
}

/* Put a free block in the list, ordered by address, so that two neighbours
 * are found by one step and the arena does not fragment into a scatter. */
static void heap_insert(struct heap_block *block)
{
    struct heap_block *cursor = g_heap_free;
    struct heap_block *before = NULL;

    while (cursor != NULL && cursor < block) {
        before = cursor;
        cursor = cursor->next;
    }

    block->next = cursor;
    block->previous = before;

    if (before != NULL) {
        before->next = block;
    } else {
        g_heap_free = block;
    }

    if (cursor != NULL) {
        cursor->previous = block;
    }
}

static void heap_remove(struct heap_block *block)
{
    if (block->previous != NULL) {
        block->previous->next = block->next;
    } else {
        g_heap_free = block->next;
    }

    if (block->next != NULL) {
        block->next->previous = block->previous;
    }

    block->next = NULL;
    block->previous = NULL;
}

static void heap_join(struct heap_block *block)
{
    struct heap_block *following = block->next;

    if (following != NULL && (unsigned char *)block + block->size == (unsigned char *)following &&
        following->free) {
        heap_remove(following);
        block->size += following->size;
    }
}

void *malloc(size_t size)
{
    struct heap_block *block;
    size_t need;

    if (size == 0) {
        return NULL;
    }

    if (!g_heap_ready) {
        heap_init();
    }

    need = heap_align(size) + heap_align(sizeof(struct heap_block));
    if (need < size) { /* the sum wrapped */
        errno = ENOMEM;
        return NULL;
    }

    for (block = g_heap_free; block != NULL; block = block->next) {
        if (block->size >= need) {
            break;
        }
    }

    if (block == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    /*
     * A remainder that cannot hold a header and the smallest payload stays
     * with the block, so a call never leaves a splinter nobody can use.
     */
    if (block->size - need >= heap_align(sizeof(struct heap_block)) + HEAP_MIN_PAYLOAD) {
        struct heap_block *rest = (struct heap_block *)((unsigned char *)block + need);

        rest->size = block->size - need;
        rest->free = 1;
        block->size = need;

        heap_insert(rest);
    }

    heap_remove(block);
    block->free = 0;

    return (void *)(block + 1);
}

void free(void *memory)
{
    struct heap_block *block;

    if (memory == NULL) {
        return;
    }

    block = (struct heap_block *)memory - 1;
    if (block->free) {
        return; /* a double free is a mistake, and not a second block */
    }

    block->free = 1;
    heap_insert(block);

    heap_join(block);

    if (block->previous != NULL && block->previous->free) {
        heap_join(block->previous);
    }
}

void *calloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *block;

    if (count != 0 && total / count != size) {
        errno = ENOMEM;
        return NULL;
    }

    block = malloc(total);
    if (block != NULL) {
        memset(block, 0, total);
    }

    return block;
}

void *realloc(void *memory, size_t size)
{
    struct heap_block *block;
    void *replacement;
    size_t payload;

    if (memory == NULL) {
        return malloc(size);
    }

    if (size == 0) {
        free(memory);
        return NULL;
    }

    block = (struct heap_block *)memory - 1;
    payload = block->size - heap_align(sizeof(struct heap_block));

    if (heap_align(size) <= payload) {
        return memory;
    }

    replacement = malloc(size);
    if (replacement == NULL) {
        return NULL;
    }

    memcpy(replacement, memory, payload);
    free(memory);

    return replacement;
}
