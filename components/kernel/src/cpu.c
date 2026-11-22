/*
 * Micro OS — the cores of the machine, as the scheduler sees them.
 *
 * The array is the kernel's own: the port says how many cores it started and
 * which one is asking, and from that the scheduler builds one record per core
 * before the first task exists. Nothing here sleeps: the boot calls it with
 * the interrupts off, and the rest of the kernel calls the lock of a core for
 * a few instructions only.
 */
#include "kernel/cpu.h"

#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"

static struct k_cpu g_cpus[KERNEL_CPUS_MAX];

/*
 * The count is written once, at the boot, and read by every tick: an atomic
 * keeps a reader that does not hold the scheduler lock (a core asking which
 * one it is) from disagreeing with the array.
 */
static k_u32_t g_cpu_count = 1;

void k_cpu_init(void)
{
    k_u32_t count = 1;
    k_u32_t i;

#if KERNEL_SMP
    count = k_port_cpu_count();

    if (count == 0u) {
        count = 1u;
    }

    if (count > KERNEL_CPUS_MAX) {
        kwarn("cpu: the port reports %u core(s), the kernel has room for %u", (unsigned)count,
              (unsigned)KERNEL_CPUS_MAX);
        count = KERNEL_CPUS_MAX;
    }
#else
    /*
     * A one-core kernel ignores what the port would report: the flag is the
     * promise of the 1.0 behaviour, not a property of the machine underneath.
     */
#endif

    memset(g_cpus, 0, sizeof(g_cpus));

    for (i = 0; i < count; i++) {
        g_cpus[i].id = i;
        g_cpus[i].online = true;
    }

    __atomic_store_n(&g_cpu_count, count, __ATOMIC_RELAXED);

    if (count > 1u) {
        kinfo("cpu: %u core(s) of the machine are used by the kernel", (unsigned)count);
    }
}

k_u32_t k_cpu_count(void)
{
    return __atomic_load_n(&g_cpu_count, __ATOMIC_RELAXED);
}

k_u32_t k_cpu_id(void)
{
#if KERNEL_SMP
    k_u32_t id = k_port_cpu_id();
    k_u32_t count = k_cpu_count();

    /* A port that names a core the kernel does not have is a bug of the
     * port; the scheduler keeps the task on the first core instead of
     * writing outside the array. */
    return (id < count) ? id : 0u;
#else
    return 0u;
#endif
}

struct k_cpu *k_cpu_self(void)
{
    return &g_cpus[k_cpu_id()];
}

struct k_cpu *k_cpu_by_id(k_u32_t id)
{
    if (id >= k_cpu_count()) {
        return NULL;
    }

    return &g_cpus[id];
}

struct task *k_cpu_current(void)
{
    /* The field is written under the scheduler lock; a reader that does not
     * hold it (a tick on the way to the charge) still gets one of the two
     * values and never a torn pointer. */
    return __atomic_load_n(&k_cpu_self()->current, __ATOMIC_RELAXED);
}

void k_cpu_lock(void)
{
    struct k_cpu *cpu;

    /*
     * The port section first: it is the spinlock with the interrupts of this
     * core saved and turned off, and the nesting is the one the port counts.
     * The kernel counts its own nesting on top, so that the scheduler can ask
     * "did I take it?" without reaching into the port.
     */
    k_port_enter_critical();

    cpu = k_cpu_self();
    cpu->interrupt_depth++;
}

void k_cpu_unlock(void)
{
    struct k_cpu *cpu = k_cpu_self();

    if (cpu->interrupt_depth > 0u) {
        cpu->interrupt_depth--;
    }

    k_port_exit_critical();
}

bool k_cpu_lock_held(void)
{
    return k_cpu_self()->interrupt_depth > 0u;
}
