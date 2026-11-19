/*
 * Micro OS — platform port: the locking.
 *
 * Two different things are needed and they are not interchangeable:
 *
 *   - a critical section: very short, no sleeping, no allocation. It
 *     protects the kernel bookkeeping (the ready queue, the registry of
 *     tasks) against the other core and against an interrupt.
 *
 *   - a mutex: may sleep, may be held across a file operation, and may be
 *     taken again by the task that already holds it. It protects a
 *     subsystem, not a variable.
 *
 * Linux draws the same line between spinlock_t and struct mutex, and the
 * port plays the role of arch/<arch>/include/asm: the kernel knows the
 * contract, the platform knows how a lock is made on this chip.
 */
#ifndef MICRO_OS_KERNEL_PORT_LOCK_H
#define MICRO_OS_KERNEL_PORT_LOCK_H

#include "kernel/types.h"

void k_port_enter_critical(void);
void k_port_exit_critical(void);

/*
 * A recursive mutex. The structure is opaque: the port owns its shape,
 * the size and the fields, exactly like struct mutex of Linux hides the
 * architecture specific parts behind its header.
 */
typedef struct k_port_mutex k_port_mutex_t;

/* Create a recursive mutex. Returns NULL when there is no memory. */
k_port_mutex_t *k_port_mutex_create(void);

/* Destroy a mutex. A locked mutex is not destroyed. */
void k_port_mutex_destroy(k_port_mutex_t *mutex);

/*
 * Take the mutex, waiting as long as it takes. The task that already owns
 * it takes it again (the depth is counted). Returns 0, or -1 for a NULL
 * mutex.
 */
int k_port_mutex_lock(k_port_mutex_t *mutex);

/* Release the mutex one level. Returns 0, or -1 for a NULL mutex. */
int k_port_mutex_unlock(k_port_mutex_t *mutex);

#endif /* MICRO_OS_KERNEL_PORT_LOCK_H */
