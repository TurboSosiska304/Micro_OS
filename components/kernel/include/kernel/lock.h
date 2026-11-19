/*
 * Micro OS — the locks of the kernel.
 *
 * A critical section (kernel/port/lock.h) is for a variable and lasts a
 * few instructions: it may not sleep and it may not allocate. A mutex is
 * for a subsystem: it is held across a file operation, it belongs to a
 * task, and the task that owns it may take it again - the filesystem
 * calls its own locked helpers all the time.
 *
 * The kernel keeps a handle and nothing else: the shape of the lock
 * belongs to the port, as struct mutex keeps the architecture specific
 * parts out of the generic header in Linux.
 */
#ifndef MICRO_OS_KERNEL_LOCK_H
#define MICRO_OS_KERNEL_LOCK_H

#include "kernel/types.h"

/*
 * A mutex. The zero value is a valid mutex that has not been used yet:
 * the handle is created on the first lock, so a static mutex needs no
 * initialisation and no construction order.
 */
typedef struct k_mutex {
    void *handle;
} k_mutex_t;

#define K_MUTEX_INITIALIZER {NULL}

/*
 * Take the mutex, waiting as long as it takes; a task that already owns
 * it takes it again. Returns 0, or -1 when the lock could not be made
 * (no memory) - the caller has to treat that as an error and not as a
 * lock it owns.
 */
int k_mutex_lock(k_mutex_t *mutex);

/* Release the mutex one level. Returns 0, or -1 for a NULL mutex. */
int k_mutex_unlock(k_mutex_t *mutex);

/*
 * Give the handle back to the port. A mutex that lives as long as the
 * kernel does not need this; a subsystem that is stopped and started
 * again, and a test, do. Returns 0, or -1 when the lock is in use - then
 * the mutex stays valid and the caller keeps it.
 */
int k_mutex_destroy(k_mutex_t *mutex);

#endif /* MICRO_OS_KERNEL_LOCK_H */
