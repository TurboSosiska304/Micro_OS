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
 * The arena is the **first** block of the heap and not the whole of it: a
 * program whose window is 800 by 600 pixels asks for two megabytes and a
 * constant of the build cannot answer it. When the list has no block big
 * enough the heap asks the kernel for more memory with `brk` (number 47),
 * which grows the address space of the program; the answer is a block of the
 * same free list, so the small allocations of a library stay fast in the arena
 * of the bss and the large ones come from the machine. A machine without an
 * address space - the board - answers `brk` with ENOSYS and the heap keeps
 * the arena it has, which is the behaviour of every release before this one.
 *
 * `free` gives a block back to the list and never to the kernel: a heap that
 * shrank would make a loop of a large allocation a call of the machine for
 * every round, and the memory of a program is given back whole when the
 * program ends. `ULIBC_HEAP_SIZE` is therefore the *first* block and not the
 * ceiling, and a program of the tree does not need to set it.
 */
#include "ulibc.h"

#include "ulibc_internal.h"

#include <errno.h>
#include <stdint.h>

#ifndef ULIBC_HEAP_SIZE
#define ULIBC_HEAP_SIZE (256u * 1024u)
#endif

#define HEAP_ALIGN 16u
#define HEAP_MIN_PAYLOAD 16u
/*
 * The smallest piece the heap asks of `brk`. A program that allocates a few
 * bytes at a time must not cost a call of the machine per block, and a
 * program that stays in its arena pays nothing at all.
 */
#define HEAP_GROW_MIN (32u * 1024u)

struct heap_block {
    size_t size; /* the block itself, its header included */
    int free;
    struct heap_block *next;
    struct heap_block *previous;
};

_Alignas(16) static unsigned char g_heap[ULIBC_HEAP_SIZE];
static struct heap_block *g_heap_free;
static int g_heap_ready;
static int g_heap_growable = 1;

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
 * are found by one step and the heap does not fragment into a scatter. */
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

/* Join a block with the free block that follows it in the address order, when
 * the two are neighbours; the caller has already inserted it in the list. */
static void heap_join_next(struct heap_block *block)
{
    struct heap_block *following = block->next;

    if (following != NULL && (unsigned char *)block + block->size == (unsigned char *)following &&
        following->free) {
        heap_remove(following);
        block->size += following->size;
    }
}

static void heap_join(struct heap_block *block)
{
    heap_join_next(block);

    if (block->previous != NULL && block->previous->free) {
        heap_join_next(block->previous);
    }
}

/*
 * One more piece of the heap, asked of the kernel. The break is read every
 * time and not kept: the program itself may have moved it with `sbrk`, and a
 * stale copy would hand out memory that somebody else owns. The address the
 * block begins at is aligned, and what the alignment skipped stays lost - it
 * is at most fifteen bytes, once.
 *
 * A `brk` that answers ENOSYS is a machine without an address space of a
 * program (the board, the simulator): the heap stops asking for more instead
 * of paying a failed call for every allocation. A refusal of the memory budget
 * of the task (ENOMEM, `ulimit -m`) is not final - the program may free
 * something and ask again - so only ENOSYS is remembered.
 */
static struct heap_block *heap_grow(size_t need)
{
    long current;
    unsigned char *base;
    unsigned char *asked;
    struct heap_block *block;
    size_t total;
    long next;

    if (!g_heap_growable) {
        return NULL;
    }

    total = (need > HEAP_GROW_MIN) ? heap_align(need) : HEAP_GROW_MIN;

    current = __ulibc_syscall6(KSYS_BRK, 0, 0, 0, 0, 0, 0);
    if (current < 0) {
        if (current == -ENOSYS) {
            g_heap_growable = 0;
        }
        return NULL;
    }

    base = (unsigned char *)heap_align((size_t)(unsigned long)current);
    asked = base + total;

    next = __ulibc_syscall6(KSYS_BRK, (long)(unsigned long)asked, 0, 0, 0, 0, 0);
    if (next < 0) {
        if (next == -ENOSYS) {
            g_heap_growable = 0;
        }
        return NULL;
    }

    block = (struct heap_block *)base;
    block->size = total;
    block->free = 1;
    block->next = NULL;
    block->previous = NULL;

    return block;
}

/* Take a block out of the free list and split what the request does not need.
 * A remainder that cannot hold a header and the smallest payload stays with
 * the block, so a call never leaves a splinter nobody can use. */
static void *heap_take(struct heap_block *block, size_t need)
{
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

    for (;;) {
        for (block = g_heap_free; block != NULL; block = block->next) {
            if (block->size >= need) {
                return heap_take(block, need);
            }
        }

        /*
         * Nothing in the list: the heap grows and the new piece joins the end
         * of what the kernel gave before, if the two are neighbours. One
         * attempt only - a second call of `brk` after a refusal would ask the
         * machine the same question twice.
         */
        if (!g_heap_growable) {
            break;
        }

        block = heap_grow(need);
        if (block == NULL) {
            break;
        }

        heap_insert(block);
        heap_join(block);
    }

    errno = ENOMEM;
    return NULL;
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
