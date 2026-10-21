#include "kernel/kmm.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/task.h"

/* The header in front of every block, so that kmm_free() knows what to do. */
struct kmm_header {
    k_u32_t magic;
    k_u32_t size;
    k_u8_t pool;     /* 0: the internal RAM, 1: the PSRAM */
    k_i8_t priority; /* the priority the block was allocated with */
    k_u8_t reserved;
    k_u8_t padding[5]; /* the header is 16 bytes, the data is aligned to 8 */
};

#define KMM_MAGIC 0x4B4D4D42u /* "KMMB" */
#define KMM_ALIGN 8u

static struct kmm_stats g_stats;

static k_size_t align_up(k_size_t value)
{
    return (value + (KMM_ALIGN - 1)) & ~((k_size_t)(KMM_ALIGN - 1));
}

void kmm_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
}

int kmm_current_priority(void)
{
    struct task *self = task_self();

    if (self == NULL) {
        /* Outside of a task: the caller is the kernel itself. */
        return KMM_PRIORITY_KERNEL;
    }

    return (int)self->mem_priority;
}

static void *alloc_from(k_size_t size, bool psram, int priority, k_u8_t pool)
{
    struct kmm_header *header;
    void *raw = k_port_alloc(size, psram);

    if (raw == NULL) {
        return NULL;
    }

    header = (struct kmm_header *)raw;
    header->magic = KMM_MAGIC;
    header->size = (k_u32_t)size;
    header->pool = pool;
    header->priority = (k_i8_t)priority;
    header->reserved = 0;

    if (pool == 0) {
        if (priority >= KMM_PRIORITY_KERNEL) {
            g_stats.kernel_bytes += size;
        } else if (priority >= KMM_PRIORITY_SYSTEM) {
            g_stats.system_bytes += size;
        } else {
            g_stats.user_bytes += size;
        }
    } else {
        g_stats.psram_bytes += size;
    }

    g_stats.allocations++;
    g_stats.total_bytes += size;

    return (void *)((uint8_t *)raw + sizeof(struct kmm_header));
}

void *kmm_alloc(k_size_t size, int priority)
{
    k_size_t total;
    void *block;

    if (size == 0) {
        return NULL;
    }

    total = align_up(size + sizeof(struct kmm_header));

    if (priority >= KMM_PRIORITY_SYSTEM) {
        /*
         * The kernel and the system utilities: the internal RAM first, the
         * PSRAM as a fallback. They must not fail because an application
         * took the memory.
         */
        block = alloc_from(total, false, priority, 0);
        if (block != NULL) {
            return block;
        }

        return alloc_from(total, true, priority, 1);
    }

    /*
     * The user: the PSRAM first. The internal RAM is only used when the
     * PSRAM is full and the reserve of the kernel is still untouched.
     */
    block = alloc_from(total, true, priority, 1);
    if (block != NULL) {
        return block;
    }

    if (k_port_heap_free() > KMM_RESERVED_INTERNAL + total) {
        return alloc_from(total, false, priority, 0);
    }

    return NULL;
}

void kmm_free(void *ptr)
{
    struct kmm_header *header;

    if (ptr == NULL) {
        return;
    }

    header = (struct kmm_header *)((uint8_t *)ptr - sizeof(struct kmm_header));

    if (header->magic != KMM_MAGIC) {
        kerror("kmm: the block %p does not belong to the memory manager", ptr);
        return;
    }

    if (header->pool == 0) {
        if (header->priority >= KMM_PRIORITY_KERNEL) {
            g_stats.kernel_bytes -= header->size;
        } else if (header->priority >= KMM_PRIORITY_SYSTEM) {
            g_stats.system_bytes -= header->size;
        } else {
            g_stats.user_bytes -= header->size;
        }
    } else {
        g_stats.psram_bytes -= header->size;
    }

    g_stats.allocations--;
    g_stats.total_bytes -= header->size;

    header->magic = 0;
    k_port_free(header);
}

void kmm_get_stats(struct kmm_stats *out)
{
    if (out == NULL) {
        return;
    }

    *out = g_stats;
}
