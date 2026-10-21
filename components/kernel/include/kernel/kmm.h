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
 */
#ifndef MICRO_OS_KERNEL_KMM_H
#define MICRO_OS_KERNEL_KMM_H

#include "kernel/types.h"

/*
 * The priorities of the allocation.
 *
 * KMM_PRIORITY_KERNEL  the kernel and the drivers: they must not fail
 * KMM_PRIORITY_SYSTEM  the system utilities, the tasks with the system flag
 * KMM_PRIORITY_USER    everything else
 */
#define KMM_PRIORITY_KERNEL 1
#define KMM_PRIORITY_SYSTEM 0
#define KMM_PRIORITY_USER (-1)

/*
 * The part of the internal RAM that the user allocations cannot touch. The
 * kernel and the drivers always keep this reserve.
 */
#ifndef KMM_RESERVED_INTERNAL
#define KMM_RESERVED_INTERNAL (48 * 1024)
#endif

struct kmm_stats {
    k_size_t kernel_bytes; /* the internal RAM used by the kernel and the drivers */
    k_size_t system_bytes; /* the internal RAM used by the system utilities */
    k_size_t user_bytes;   /* the internal RAM used by the user applications */
    k_size_t psram_bytes;  /* the PSRAM used by everybody */
    k_size_t allocations;  /* the number of the living blocks */
    k_size_t total_bytes;  /* the total of the allocated bytes */
};

/* Prepare the memory manager. */
void kmm_init(void);

/*
 * Allocate a block. The priority decides where the memory comes from:
 * the internal RAM for the kernel and the system, the PSRAM for the user.
 */
void *kmm_alloc(k_size_t size, int priority);

/* Free a block that came from kmm_alloc(). */
void kmm_free(void *ptr);

/* The priority of the current task (KMM_PRIORITY_USER outside of a task). */
int kmm_current_priority(void);

/* The statistics of the manager. */
void kmm_get_stats(struct kmm_stats *out);

#endif /* MICRO_OS_KERNEL_KMM_H */
