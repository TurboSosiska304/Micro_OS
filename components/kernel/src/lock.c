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

#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/port.h"
#include "kernel/port/cpu.h"
#include "kernel/port/lock.h"
#include "kernel/port_task.h"
#include "kernel/task.h"

#define MUTEX_DBG_MAX 64

struct mutex_dbg_entry {
    const k_mutex_t *key;
    const char *name;
    k_u32_t owner_pid;
    k_u16_t depth;
};

static struct mutex_dbg_entry g_mutex_dbg[MUTEX_DBG_MAX];
static k_u16_t g_mutex_dbg_count;

static void mutex_dbg_register(k_mutex_t *mutex)
{
    k_u16_t i;

    if (mutex == NULL) {
        return;
    }

    k_port_enter_critical();
    for (i = 0; i < g_mutex_dbg_count; i++) {
        if (g_mutex_dbg[i].key == mutex) {
            g_mutex_dbg[i].name = mutex->name;
            k_port_exit_critical();
            return;
        }
    }
    if (g_mutex_dbg_count < MUTEX_DBG_MAX) {
        k_u16_t slot = g_mutex_dbg_count;
        g_mutex_dbg[slot].key = mutex;
        g_mutex_dbg[slot].name = mutex->name;
        g_mutex_dbg[slot].owner_pid = 0u;
        g_mutex_dbg[slot].depth = 0u;
        __atomic_store_n(&g_mutex_dbg_count, (k_u16_t)(slot + 1u), __ATOMIC_RELEASE);
    }
    k_port_exit_critical();
}

static void mutex_dbg_update(k_mutex_t *mutex, k_u32_t owner_pid, k_u16_t depth)
{
    k_u16_t count = __atomic_load_n(&g_mutex_dbg_count, __ATOMIC_RELAXED);
    k_u16_t i;

    if (mutex == NULL) {
        return;
    }

    if (count > MUTEX_DBG_MAX) {
        count = MUTEX_DBG_MAX;
    }

    for (i = 0; i < count; i++) {
        if (g_mutex_dbg[i].key == mutex) {
            __atomic_store_n(&g_mutex_dbg[i].owner_pid, owner_pid, __ATOMIC_RELAXED);
            __atomic_store_n(&g_mutex_dbg[i].depth, depth, __ATOMIC_RELAXED);
            g_mutex_dbg[i].name = mutex->name;
            return;
        }
    }
}

k_size_t k_mutex_debug_snapshot(struct k_mutex_debug_info *out, k_size_t max)
{
    k_size_t n = 0;
    k_u16_t count = __atomic_load_n(&g_mutex_dbg_count, __ATOMIC_RELAXED);
    k_u16_t i;

    if (out == NULL || max == 0u) {
        return 0u;
    }

    if (count > MUTEX_DBG_MAX) {
        count = MUTEX_DBG_MAX;
    }

    for (i = 0; i < count && n < max; i++) {
        out[n].name = g_mutex_dbg[i].name;
        out[n].owner_pid = __atomic_load_n(&g_mutex_dbg[i].owner_pid, __ATOMIC_RELAXED);
        out[n].depth = __atomic_load_n(&g_mutex_dbg[i].depth, __ATOMIC_RELAXED);
        n++;
    }

    return n;
}

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

    if (mutex->handle != NULL) {
        mutex_dbg_register(mutex);
    }

    return (k_port_mutex_t *)mutex->handle;
}

int k_mutex_lock(k_mutex_t *mutex)
{
    k_port_mutex_t *handle;
    k_u32_t depth = k_port_critical_depth();
    struct task *self;

    if (depth > 0u) {
        struct task *t = k_port_task_current();
        kerror("lock: [LOCKORDER] cpu %u depth %u task %s taking mutex %p",
               (unsigned)k_port_cpu_id(), (unsigned)depth, t != NULL ? t->name : "<none>",
               (void *)mutex);
    }

    handle = mutex_handle(mutex);

    if (handle == NULL) {
        return -1;
    }

    if (k_port_mutex_lock(handle) != 0) {
        return -1;
    }

    self = k_port_task_current();
    __atomic_store_n(&mutex->owner_pid, self != NULL ? self->pid : 0u, __ATOMIC_RELAXED);
    {
        k_u16_t d = __atomic_add_fetch(&mutex->depth, 1u, __ATOMIC_RELAXED);
        mutex_dbg_update(mutex, __atomic_load_n(&mutex->owner_pid, __ATOMIC_RELAXED), d);
    }
    return 0;
}

int k_mutex_unlock(k_mutex_t *mutex)
{
    k_u16_t prev;

    if (mutex == NULL || mutex->handle == NULL) {
        return -1;
    }

    prev = __atomic_load_n(&mutex->depth, __ATOMIC_RELAXED);
    if (prev > 0u) {
        k_u16_t now = __atomic_sub_fetch(&mutex->depth, 1u, __ATOMIC_RELAXED);
        if (now == 0u) {
            __atomic_store_n(&mutex->owner_pid, 0u, __ATOMIC_RELAXED);
        }
        mutex_dbg_update(mutex, __atomic_load_n(&mutex->owner_pid, __ATOMIC_RELAXED), now);
    }

    return k_port_mutex_unlock((k_port_mutex_t *)mutex->handle);
}

int k_mutex_destroy(k_mutex_t *mutex)
{
    k_u16_t i;

    if (mutex == NULL || mutex->handle == NULL) {
        return -1;
    }

    if (k_port_mutex_destroy((k_port_mutex_t *)mutex->handle) != 0) {
        /* The lock is held: it stays, and the caller keeps it. */
        return -1;
    }

    k_port_enter_critical();
    for (i = 0; i < g_mutex_dbg_count; i++) {
        if (g_mutex_dbg[i].key == mutex) {
            g_mutex_dbg[i] = g_mutex_dbg[g_mutex_dbg_count - 1u];
            g_mutex_dbg[g_mutex_dbg_count - 1u].key = NULL;
            g_mutex_dbg[g_mutex_dbg_count - 1u].name = NULL;
            g_mutex_dbg[g_mutex_dbg_count - 1u].owner_pid = 0u;
            g_mutex_dbg[g_mutex_dbg_count - 1u].depth = 0u;
            __atomic_store_n(&g_mutex_dbg_count, (k_u16_t)(g_mutex_dbg_count - 1u), __ATOMIC_RELEASE);
            break;
        }
    }
    k_port_exit_critical();

    mutex->handle = NULL;
    __atomic_store_n(&mutex->owner_pid, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&mutex->depth, 0u, __ATOMIC_RELAXED);

    return 0;
}
