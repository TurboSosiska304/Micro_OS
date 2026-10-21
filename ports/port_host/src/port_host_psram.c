/*
 * Micro OS — the PSRAM of the host port.
 *
 * The host has no separate PSRAM, but the tests need one: the priority of
 * the allocations decides where the memory comes from, and that can only
 * be checked when the two pools are really different.
 *
 * The memory is a plain array with a list of the free blocks. The blocks
 * that are given back are merged with their neighbours, so that the memory
 * is really reused: without the merge the pool would leak a little with
 * every free, and the tests would run out of the PSRAM for no reason.
 */
#include "port_host_psram.h"

#include <string.h>

#define HOST_PSRAM_SIZE (2 * 1024 * 1024)
#define HOST_PSRAM_MAX_BLOCKS 128
#define HOST_PSRAM_ALIGN 8u

struct free_block {
    k_size_t offset;
    k_size_t size;
};

/* The blocks that are given out: their size is needed to give them back. */
struct alloc_block {
    void *ptr;
    k_size_t size;
};

static _Alignas(16) uint8_t s_pool[HOST_PSRAM_SIZE];
static struct free_block s_free[HOST_PSRAM_MAX_BLOCKS];
static struct alloc_block s_alloc[HOST_PSRAM_MAX_BLOCKS];
static k_size_t s_free_count;
static k_size_t s_alloc_count;
static bool s_initialised;

static k_size_t align_up(k_size_t value)
{
    return (value + (HOST_PSRAM_ALIGN - 1)) & ~((k_size_t)(HOST_PSRAM_ALIGN - 1));
}

void host_psram_init(void)
{
    s_free_count = 1;
    s_free[0].offset = 0;
    s_free[0].size = HOST_PSRAM_SIZE;
    s_alloc_count = 0;
    s_initialised = true;
}

void *host_psram_alloc(k_size_t size)
{
    k_size_t i;

    if (!s_initialised) {
        host_psram_init();
    }

    if (size == 0) {
        return NULL;
    }

    size = align_up(size);

    for (i = 0; i < s_free_count; i++) {
        if (s_free[i].size >= size) {
            void *result = &s_pool[s_free[i].offset];

            s_free[i].offset += size;
            s_free[i].size -= size;

            if (s_free[i].size == 0) {
                /* The block is used up, remove it from the list. */
                for (; i + 1 < s_free_count; i++) {
                    s_free[i] = s_free[i + 1];
                }
                s_free_count--;
            }

            /* Remember the block, to be able to give the memory back. */
            if (s_alloc_count < HOST_PSRAM_MAX_BLOCKS) {
                s_alloc[s_alloc_count].ptr = result;
                s_alloc[s_alloc_count].size = size;
                s_alloc_count++;
            }

            return result;
        }
    }

    return NULL;
}

void host_psram_release(void *ptr)
{
    k_size_t offset;
    k_size_t size = 0;
    k_size_t i;
    k_size_t j;

    if (ptr == NULL) {
        return;
    }

    offset = (k_size_t)((uint8_t *)ptr - s_pool);

    /* The size of the block: it is remembered at the allocation. */
    for (i = 0; i < s_alloc_count; i++) {
        if (s_alloc[i].ptr == ptr) {
            size = s_alloc[i].size;
            for (; i + 1 < s_alloc_count; i++) {
                s_alloc[i] = s_alloc[i + 1];
            }
            s_alloc_count--;
            break;
        }
    }

    if (size == 0 || s_free_count >= HOST_PSRAM_MAX_BLOCKS) {
        return;
    }

    /* The place of the block in the sorted list. */
    for (i = 0; i < s_free_count; i++) {
        if (offset < s_free[i].offset) {
            break;
        }
    }

    /* The merge with the block that follows. */
    if (i < s_free_count && offset + size == s_free[i].offset) {
        size += s_free[i].size;
        for (; i + 1 < s_free_count; i++) {
            s_free[i] = s_free[i + 1];
        }
        s_free_count--;
    }

    /* The merge with the block that goes before. */
    if (i > 0 && s_free[i - 1].offset + s_free[i - 1].size == offset) {
        s_free[i - 1].size += size;
        return;
    }

    /* The insert. */
    for (j = s_free_count; j > i; j--) {
        s_free[j] = s_free[j - 1];
    }
    s_free[i].offset = offset;
    s_free[i].size = size;
    s_free_count++;
}

k_size_t host_psram_total(void)
{
    return HOST_PSRAM_SIZE;
}

k_size_t host_psram_available(void)
{
    k_size_t i;
    k_size_t free_bytes = 0;

    for (i = 0; i < s_free_count; i++) {
        free_bytes += s_free[i].size;
    }

    return free_bytes;
}

bool host_psram_contains(const void *ptr)
{
    const uint8_t *p = ptr;

    return p >= s_pool && p < (s_pool + HOST_PSRAM_SIZE);
}
