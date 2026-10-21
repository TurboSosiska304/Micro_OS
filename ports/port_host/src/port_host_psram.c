/*
 * Micro OS — the PSRAM of the host port.
 *
 * The host has no separate PSRAM, but the tests need one: the priority of
 * the allocations decides where the memory comes from, and that can only
 * be checked when the two pools are really different.
 */
#include "port_host_psram.h"

#include <string.h>

#define HOST_PSRAM_SIZE (2 * 1024 * 1024)
#define HOST_PSRAM_MAX_BLOCKS 64

struct free_block {
    k_size_t offset;
    k_size_t size;
};

static uint8_t s_pool[HOST_PSRAM_SIZE];
static struct free_block s_free[HOST_PSRAM_MAX_BLOCKS];
static k_size_t s_free_count;
static bool s_initialised;

void host_psram_init(void)
{
    s_free_count = 1;
    s_free[0].offset = 0;
    s_free[0].size = HOST_PSRAM_SIZE;
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

            return result;
        }
    }

    return NULL;
}

void host_psram_release(void *ptr)
{
    k_size_t offset;
    k_size_t i;

    if (ptr == NULL || s_free_count >= HOST_PSRAM_MAX_BLOCKS) {
        return;
    }

    offset = (k_size_t)((uint8_t *)ptr - s_pool);

    /* Find the place of the block, the list is sorted by the offset. */
    for (i = 0; i < s_free_count; i++) {
        if (offset < s_free[i].offset) {
            break;
        }
    }

    for (; s_free_count > i; i++) {
        s_free[i] = s_free[i - 1];
    }

    s_free[i].offset = offset;
    s_free[i].size = 0;
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
    const uint8_t *p = (const uint8_t *)ptr;

    return p >= s_pool && p < (s_pool + HOST_PSRAM_SIZE);
}
