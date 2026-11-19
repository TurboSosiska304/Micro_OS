/*
 * Micro OS — the mutex of the kernel.
 *
 * The handle is made on the first lock, so a static mutex needs no
 * initialisation and no construction order: the filesystem and the telnet
 * server have a lock each, and both may be used before a task that could
 * initialise them exists.
 *
 * The creation happens outside the critical section. The heap has a lock
 * of its own, and taking a spinlock with interrupts off around a call
 * that may wait for another core is how a deadlock is made; the two
 * checks and one assignment under the spinlock are enough to let exactly
 * one task win, and the loser throws its own handle away.
 */
#include "kernel/lock.h"

#include "kernel/port.h"

static k_port_mutex_t *mutex_handle(k_mutex_t *mutex)
{
    k_port_mutex_t *fresh;

    if (mutex == NULL) {
        return NULL;
    }

    if (mutex->handle != NULL) {
        return (k_port_mutex_t *)mutex->handle;
    }

    fresh = k_port_mutex_create();
    if (fresh == NULL) {
        return NULL;
    }

    k_port_enter_critical();

    if (mutex->handle == NULL) {
        mutex->handle = fresh;
        fresh = NULL;
    }

    k_port_exit_critical();

    if (fresh != NULL) {
        k_port_mutex_destroy(fresh);
    }

    return (k_port_mutex_t *)mutex->handle;
}

int k_mutex_lock(k_mutex_t *mutex)
{
    k_port_mutex_t *handle = mutex_handle(mutex);

    if (handle == NULL) {
        return -1;
    }

    return k_port_mutex_lock(handle);
}

int k_mutex_unlock(k_mutex_t *mutex)
{
    if (mutex == NULL || mutex->handle == NULL) {
        return -1;
    }

    return k_port_mutex_unlock((k_port_mutex_t *)mutex->handle);
}
