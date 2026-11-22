/*
 * Micro OS — the cores of the machine.
 *
 * The scheduler of 1.0 had one `current`, one ready queue and one idle task,
 * because one processor was the whole machine (ADR 0012). A `struct k_cpu`
 * is the record of one core: what runs on it, what it counts and what the
 * port had to build for it. The array of them lives here and not in the port,
 * because the scheduler reads it on every tick; the port only says how many
 * cores it has and which one is asking (kernel/port/cpu.h).
 *
 * With KERNEL_SMP off there is exactly one core in the array, its slot is the
 * only one the scheduler ever touches, and the kernel is the kernel of 1.0.
 */
#ifndef MICRO_OS_KERNEL_CPU_H
#define MICRO_OS_KERNEL_CPU_H

#include "kernel/config.h"
#include "kernel/types.h"

struct task;

/*
 * The number of the slots. A build without SMP has one, and the static
 * memory of a build with it is the per-CPU run queue of the scheduler; four
 * cores cover the machines of the project (the two of the board, the two of
 * the smoke test) with room for a machine of a person.
 */
#if KERNEL_SMP
#ifndef KERNEL_CPUS_MAX
#define KERNEL_CPUS_MAX 4
#endif
#else
#define KERNEL_CPUS_MAX 1
#endif

/*
 * One core. The fields are the ones ADR 0012 names; the ones the port owns
 * are opaque here and are set through the port, not by the scheduler.
 *
 * The run queue of the core is the one field that is not here: its type (the
 * array of the ready tasks of a priority) belongs to the scheduler, and it
 * lives beside this array in kernel/sched.c, indexed by the same id - the way
 * Linux keeps `struct rq` in kernel/sched beside the per-CPU area of the
 * architecture. The idle task is a slot and not a promise: a port that idles
 * in its own loop (all three of this tree) leaves it NULL.
 */
struct k_cpu {
    k_u32_t id;              /* the number the scheduler and /proc use */
    struct task *current;    /* the task that runs on this core, NULL when idle */
    struct task *idle;       /* the idle task of the core, when the port made one */
    k_u32_t ticks;           /* the ticks this core has taken since the boot */
    k_u32_t interrupt_depth; /* the nesting of the sections the kernel took here */
    k_u64_t interrupt_flags; /* the flags of the outermost of them */
    void *kernel_stack;      /* the stack of the idle loop of the core */
    void *tls;               /* the thread pointer of the core (IA32_FS_BASE) */
    void *arch;              /* GDT, IDT, TSS and the other tables of the port */
    bool online;             /* the core is up and may be given a task */
};

/*
 * Prepare the array from the core count of the port. Called by
 * k_sched_init() before the first task exists, under the scheduler lock.
 */
void k_cpu_init(void);

/* The number of cores the kernel uses (1 without KERNEL_SMP). */
k_u32_t k_cpu_count(void);

/* The core that is asking. Outside a task, and on a port without SMP, it is 0. */
k_u32_t k_cpu_id(void);

/* The record of this core, and of the core with that number (NULL when there
 * is no such core). */
struct k_cpu *k_cpu_self(void);
struct k_cpu *k_cpu_by_id(k_u32_t id);

/*
 * The record of the task that runs on this core. Outside a task it is NULL:
 * a tick of the clock of a port may arrive with no task under it.
 */
struct task *k_cpu_current(void);

/*
 * The scheduler lock: the critical section of the port, which is a spinlock
 * with the interrupts of the local core saved and turned off, plus the count
 * of the nesting that the kernel itself took. It is never held while the
 * kernel sleeps, copies to a user or waits for a device; the functions
 * that do those things drop it first.
 */
void k_cpu_lock(void);
void k_cpu_unlock(void);

/* Answer whether this context holds the scheduler lock (a debug aid for the
 * scheduler and the tests; the port is the owner of the real depth). */
bool k_cpu_lock_held(void);

#endif /* MICRO_OS_KERNEL_CPU_H */
