/*
 * Micro OS — memory manager.
 *
 * The memory of the system is not divided evenly: the kernel and the drivers
 * have to work no matter what the applications do, so they get the internal
 * RAM first. The system utilities get what is left, the user applications
 * are served from the PSRAM and only get the leftovers of the internal RAM.
 *
 * If the internal RAM is tight, the allocation is decided by the priority:
 * the higher the priority, the earlier the memory is given out.
 *
 * The small blocks (up to KMM_SLAB_MAX) come from the slabs: one slab is
 * cut into slots of the same size, and the free slots are kept in a list.
 * That way a thousand small blocks do not cost a thousand calls of the
 * allocator, and the fragments of the freed blocks are not lost.
 */
#include "kernel/kmm.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"

/* The header in front of every block, so that kmm_free() knows what to do. */
struct kmm_header {
    k_u32_t magic;
    k_u32_t size;    /* the size that was asked for */
    k_u32_t slab;    /* the number of the slab, KMM_NO_SLAB for a large block */
    k_u8_t pool;     /* 0: the internal RAM, 1: the PSRAM */
    k_i8_t priority; /* the priority the block was allocated with */
    k_u8_t class;    /* the class of the slab, KMM_LARGE_CLASS for a large block */
    k_u8_t flags;    /* KMM_FLAG_FREE while the slot is free */
};

#define KMM_MAGIC 0x4B4D4D42u /* "KMMB" */
#define KMM_ALIGN 8u
#define KMM_NO_SLAB 0xFFFFFFFFu
#define KMM_LARGE_CLASS 0xFFu
#define KMM_FLAG_FREE 1u

/* The size of one slab and the number of the slabs of the system. */
#define KMM_SLAB_BYTES (8 * 1024)
#define KMM_MAX_SLABS 48

/* One slab of the memory: the slots and the free list of the slots. */
struct kmm_slab {
    struct kmm_slab *next; /* the next slab of the same class */
    void *free_slots;      /* the head of the list of the free slots */
    k_u32_t index;
    k_u16_t slots;      /* the number of the slots */
    k_u16_t free_count; /* the number of the free slots */
    k_u8_t class;
    k_u8_t pool;
    k_i8_t priority;
    k_u8_t in_use;
    k_size_t slot_size; /* the size of a slot with its header */
    void *memory;
};

static struct kmm_stats g_stats;

/* The slabs of the system. */
static struct kmm_slab g_slabs[KMM_MAX_SLABS];
static k_size_t g_slab_count;

/* The slabs of every class, for every pool. */
static struct kmm_slab *g_class_slabs[2][KMM_SLAB_CLASSES];

/*
 * The allocator is one piece: two tasks inside the slabs at the same
 * time take the same free slot, and the same block comes out for two
 * owners - the memory of the kernel falls apart quietly and shows up
 * much later somewhere else, where nothing points at the allocator.
 * The lock is of the same shape as the lock of the MFS (see mfs.c):
 * the owner is remembered, a waiter sleeps a millisecond and looks
 * again, and before the first task exists there is nobody to compete
 * with. Only the flag is touched under the critical section of the
 * port: the work of the allocator itself never sleeps and never runs
 * with the interrupts off.
 */
static struct task *s_lock_owner;
static k_u32_t s_lock_depth;

static void kmm_lock(void)
{
    struct task *self = task_self();

    if (self == NULL) {
        return;
    }

    for (;;) {
        bool taken = false;

        k_port_enter_critical();

        if (s_lock_owner == NULL) {
            s_lock_owner = self;
            s_lock_depth = 1;
            taken = true;
        } else if (s_lock_owner == self) {
            /* The lock is recursive: the functions call each other. */
            s_lock_depth++;
            taken = true;
        }

        k_port_exit_critical();

        if (taken) {
            return;
        }

        k_sched_sleep_ms(1);
    }
}

static void kmm_unlock(void)
{
    struct task *self = task_self();

    if (self == NULL) {
        return;
    }

    k_port_enter_critical();

    if (s_lock_owner == self) {
        s_lock_depth--;

        if (s_lock_depth == 0) {
            s_lock_owner = NULL;
        }
    }

    k_port_exit_critical();
}

/* The bodies of the public functions, with the lock already taken. */
static void kmm_init_locked(void);
static void *kmm_alloc_locked(k_size_t size, int priority);
static void kmm_free_locked(void *ptr);
static k_size_t kmm_block_size_locked(void *ptr);
static void kmm_get_stats_locked(struct kmm_stats *out);

void kmm_init(void)
{
    kmm_lock();
    kmm_init_locked();
    kmm_unlock();
}

void *kmm_alloc(k_size_t size, int priority)
{
    void *block;

    kmm_lock();
    block = kmm_alloc_locked(size, priority);
    kmm_unlock();

    return block;
}

void kmm_free(void *ptr)
{
    kmm_lock();
    kmm_free_locked(ptr);
    kmm_unlock();
}

k_size_t kmm_block_size(void *ptr)
{
    k_size_t size;

    kmm_lock();
    size = kmm_block_size_locked(ptr);
    kmm_unlock();

    return size;
}

void kmm_get_stats(struct kmm_stats *out)
{
    kmm_lock();
    kmm_get_stats_locked(out);
    kmm_unlock();
}

static k_size_t align_up(k_size_t value)
{
    return (value + (KMM_ALIGN - 1)) & ~((k_size_t)(KMM_ALIGN - 1));
}

/* The size of the data of a class. */
static k_size_t class_size(int class)
{
    return (k_size_t)KMM_SLAB_MIN << class;
}

/* The smallest class that holds the size. */
static int size_class(k_size_t size)
{
    int class = 0;

    while (class < KMM_SLAB_CLASSES - 1 && size > class_size(class)) {
        class ++;
    }

    return class;
}

static void kmm_init_locked(void)
{
    k_size_t i;

    /* The slabs of the previous life go back to the memory of the system. */
    for (i = 0; i < g_slab_count; i++) {
        if (g_slabs[i].in_use && g_slabs[i].memory != NULL) {
            k_port_free(g_slabs[i].memory);
        }
    }

    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_slabs, 0, sizeof(g_slabs));
    memset(g_class_slabs, 0, sizeof(g_class_slabs));
    g_slab_count = 0;
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

/* The memory of a block is counted for the pool and the priority. */
static void account_alloc(k_size_t size, int priority, k_u8_t pool)
{
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
}

static void account_free(k_size_t size, int priority, k_u8_t pool)
{
    if (pool == 0) {
        if (priority >= KMM_PRIORITY_KERNEL) {
            g_stats.kernel_bytes -= size;
        } else if (priority >= KMM_PRIORITY_SYSTEM) {
            g_stats.system_bytes -= size;
        } else {
            g_stats.user_bytes -= size;
        }
    } else {
        g_stats.psram_bytes -= size;
    }

    g_stats.allocations--;
    g_stats.total_bytes -= size;
}

/* --- the slabs --- */

/* A new slab of the class: the memory comes from the pool. */
static struct kmm_slab *new_slab(int class, k_u8_t pool, int priority)
{
    struct kmm_slab *slab;
    k_size_t slot_size = class_size(class) + sizeof(struct kmm_header);
    k_size_t slots;
    void *memory;
    k_size_t i;

    if (g_slab_count >= KMM_MAX_SLABS) {
        return NULL;
    }

    memory = k_port_alloc(KMM_SLAB_BYTES, pool == 1);
    if (memory == NULL) {
        return NULL;
    }

    slab = &g_slabs[g_slab_count];
    memset(slab, 0, sizeof(*slab));

    slots = KMM_SLAB_BYTES / slot_size;

    slab->index = (k_u32_t)g_slab_count;
    slab->class = (k_u8_t) class;
    slab->pool = pool;
    slab->priority = (k_i8_t)priority;
    slab->slot_size = slot_size;
    slab->slots = (k_u16_t)slots;
    slab->free_count = (k_u16_t)slots;
    slab->memory = memory;
    slab->in_use = 1;

    /* The free slots are chained through their data. */
    slab->free_slots = NULL;
    for (i = slots; i > 0; i--) {
        void *slot = (uint8_t *)memory + (i - 1) * slot_size;
        void *data = (uint8_t *)slot + sizeof(struct kmm_header);

        *(void **)data = slab->free_slots;
        slab->free_slots = data;
    }

    slab->next = g_class_slabs[pool][class];
    g_class_slabs[pool][class] = slab;

    g_slab_count++;
    g_stats.slabs++;
    g_stats.slab_free_bytes += slots * class_size(class);

    return slab;
}

/* A slab of the class with a free slot, NULL if there is none. */
static struct kmm_slab *find_slab(int class, k_u8_t pool)
{
    struct kmm_slab *slab = g_class_slabs[pool][class];

    while (slab != NULL) {
        if (slab->free_count > 0) {
            return slab;
        }
        slab = slab->next;
    }

    return NULL;
}

/* A small block: a slot of a slab. */
static void *alloc_small(k_size_t size, int priority, k_u8_t pool)
{
    struct kmm_slab *slab;
    struct kmm_header *header;
    void *data;

    slab = find_slab(size_class(size), pool);
    if (slab == NULL) {
        slab = new_slab(size_class(size), pool, priority);
        if (slab == NULL) {
            return NULL;
        }
    }

    data = slab->free_slots;
    slab->free_slots = *(void **)data;
    slab->free_count--;

    header = (struct kmm_header *)((uint8_t *)data - sizeof(struct kmm_header));
    header->magic = KMM_MAGIC;
    header->size = (k_u32_t)size;
    header->slab = slab->index;
    header->pool = pool;
    header->priority = (k_i8_t)priority;
    header->class = (k_u8_t)size_class(size);
    header->flags = 0;

    g_stats.slab_free_bytes -= class_size(header->class);
    account_alloc(size, priority, pool);

    return data;
}

/* --- the allocator --- */

static void *kmm_alloc_locked(k_size_t size, int priority)
{
    k_size_t total;
    void *block;
    k_u8_t pool = 1; /* the PSRAM, the default for the user */

    if (size == 0) {
        return NULL;
    }

    if (size <= KMM_SLAB_MAX) {
        /* A small block: it comes from a slab. */
        if (priority >= KMM_PRIORITY_SYSTEM) {
            block = alloc_small(size, priority, 0);
            if (block != NULL) {
                return block;
            }

            return alloc_small(size, priority, 1);
        }

        block = alloc_small(size, priority, 1);
        if (block != NULL) {
            return block;
        }

        if (k_port_heap_free() > KMM_RESERVED_INTERNAL + size) {
            return alloc_small(size, priority, 0);
        }

        return NULL;
    }

    /* A large block: it comes from the allocator of the port. */
    total = align_up(size + sizeof(struct kmm_header));

    if (priority >= KMM_PRIORITY_SYSTEM) {
        /*
         * The kernel and the system utilities: the internal RAM first, the
         * PSRAM as a fallback. They must not fail because an application
         * took the memory.
         */
        block = k_port_alloc(total, false);
        if (block != NULL) {
            pool = 0;
            goto found;
        }

        block = k_port_alloc(total, true);
        if (block != NULL) {
            pool = 1;
            goto found;
        }

        return NULL;
    }

    /*
     * The user: the PSRAM first. The internal RAM is only used when the
     * PSRAM is full and the reserve of the kernel is still untouched.
     */
    block = k_port_alloc(total, true);
    if (block != NULL) {
        pool = 1;
        goto found;
    }

    if (k_port_heap_free() > KMM_RESERVED_INTERNAL + total) {
        block = k_port_alloc(total, false);
        if (block != NULL) {
            pool = 0;
            goto found;
        }
    }

    return NULL;

found:
    {
        struct kmm_header *header = (struct kmm_header *)block;

        header->magic = KMM_MAGIC;
        header->size = (k_u32_t)size;
        header->slab = KMM_NO_SLAB;
        header->pool = pool;
        header->priority = (k_i8_t)priority;
        header->class = KMM_LARGE_CLASS;
        header->flags = 0;
    }

    account_alloc(size, priority, pool);

    return (void *)((uint8_t *)block + sizeof(struct kmm_header));
}

static void kmm_free_locked(void *ptr)
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

    if (header->class != KMM_LARGE_CLASS) {
        /* A slot of a slab: it goes back to the list of the free slots. */
        struct kmm_slab *slab = &g_slabs[header->slab];

        *(void **)ptr = slab->free_slots;
        slab->free_slots = ptr;
        slab->free_count++;

        g_stats.slab_free_bytes += class_size(header->class);
    }

    header->magic = 0;
    header->flags = KMM_FLAG_FREE;

    account_free(header->size, header->priority, header->pool);

    if (header->class == KMM_LARGE_CLASS) {
        k_port_free(header);
    }
}

static k_size_t kmm_block_size_locked(void *ptr)
{
    struct kmm_header *header;

    if (ptr == NULL) {
        return 0;
    }

    header = (struct kmm_header *)((uint8_t *)ptr - sizeof(struct kmm_header));

    if (header->magic != KMM_MAGIC) {
        return 0;
    }

    return (k_size_t)header->size;
}

static void kmm_get_stats_locked(struct kmm_stats *out)
{
    if (out == NULL) {
        return;
    }

    *out = g_stats;
}
