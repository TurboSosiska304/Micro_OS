/*
 * Micro OS — the x86-64 port: the tasks.
 *
 * The kernel decides which task runs (components/kernel/src/sched.c); the
 * port owns the execution context (ADR 0002). On this machine a context is a
 * stack and the callee-saved registers inside it, and a switch is
 * port_x86_64_switch() from switch.S. This file is the substrate around that
 * switch: the stacks, the first frame of a task, and the decisions about
 * when to switch at all.
 *
 * Two points are worth knowing before reading the code.
 *
 * First, the interrupt state travels with the context, not with the machine.
 * A context that gives up the CPU is suspended either in task code (where
 * the interrupts were on) or inside an interrupt handler (where they are
 * off until its iretq restores the flags of the interrupted code), and it
 * has to come back with the same state it left. Every switch therefore goes
 * through a pair of save_flags_and_cli() and restore_flags() whose value
 * lives on the stack of the context that made the call, so each context
 * restores its own: a task turns the interrupts on, a suspended handler
 * keeps them off until it can finish.
 *
 * Second, a task starts as the return of a switch, not as a call. There is
 * no caller above it to pass arguments and no saved context below it, so the
 * frame is built by hand (see stack_build()) and the arguments travel in the
 * callee-saved registers that the switch restores anyway.
 */
#include "port_x86_64_task.h"

#include <string.h>

#include "kernel/cpu.h"
#include "kernel/log.h"
#include "kernel/port/cpu.h"
#include "kernel/port/lock.h"
#include "kernel/port/mem.h"
#include "kernel/port/time.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_x86_64.h"
#include "port_x86_64_apic.h"
#include "port_x86_64_smp.h"

/*
 * The image of the floating point state (FXSAVE): the x87 unit, the sixteen
 * XMM registers and MXCSR, 512 bytes, and the processor wants the area on a
 * 16-byte boundary. The allocator of the port hands out payloads aligned for
 * its own bookkeeping and not for FXSAVE, so the member carries 15 bytes of
 * slack and fpu_area() rounds the address inside it up. The state lives with
 * the port and not in struct task: the kernel's record is shared by every
 * port, and the board has a hundred kilobytes of SRAM while this is 527
 * bytes of the 32 KiB stack block that a context of the x86-64 machine
 * already owns (about 17 KiB over the full table of thirty-two tasks, no
 * new allocation, and nothing at all on the board).
 */
#define PORT_FPU_AREA 512u
#define PORT_FPU_ALIGN 16u

/*
 * One execution context. It lives at the bottom of the stack it describes:
 * a task is one allocation, and the context goes away together with its
 * stack when the task is released.
 */
struct port_task {
    struct task *task; /* the kernel side; NULL for the context of the boot */
    k_u64_t rsp;       /* the stack pointer while the context is suspended */
    k_u64_t flags;     /* the RFLAGS a context that has never run starts with */
    void *block;       /* the allocation: this header and the stack above it */
    k_u8_t *tls;       /* the block behind IA32_FS_BASE of this task */
    k_u64_t stack_top; /* the empty top of the stack: RSP0 when it is a program */
    k_u64_t guard;     /* the page under the stack that is not present, 0 none */
    k_u64_t cr3;       /* the page tables of the task; the kernel's by default */
    bool is_user;      /* a program of ring 3 runs on this context */
    bool exiting;      /* k_port_task_exit has begun on this context */
    /*
     * The window of a copy into the memory of a program is open on this
     * context (ADR 0052). AC is a flag of the core and the switch does not
     * carry it, but a read or a write of a program may sleep inside its
     * window - a pipe with no room, a terminal with no key - so the flag
     * travels with the context that opened it and is put back when that
     * context comes back.
     */
    bool copying;
    /* The pages this context copied on write after a fork (ADR 0057): the
     * count of the child of a fork that is in the log when it ends. */
    k_u32_t copied_pages;
    /*
     * The x87 and XMM state of the context, saved when it leaves the CPU and
     * loaded when it comes back (see commit_switch()). The extra bytes are
     * the alignment: fpu_area() finds the 512 bytes inside them.
     */
    k_u8_t fpu[PORT_FPU_AREA + PORT_FPU_ALIGN];
};

/*
 * The block of the C library's _Thread_local variables. The linker puts it
 * between __tls_start and __tls_end, and the compiler addresses it as an
 * offset from the end, so a task gets a copy of the same bytes and points the
 * thread pointer at its own end. Without the copy two tasks share one errno,
 * and a program of ring 3 (which leaves the pointer at zero) would carry its
 * zero into the kernel of the next call.
 */
extern char __tls_start[];
extern char __tls_data_end[];
extern char __tls_end[];

static k_size_t task_tls_size(void)
{
    return (k_size_t)(__tls_end - __tls_start);
}

/*
 * The initialised part is copied and the rest is zeroed: the tbss of the
 * image is NOBITS and the loader clears only .bss, so a new task must not
 * inherit whatever the bytes after the image happen to hold.
 */
static void task_tls_copy(k_u8_t *block)
{
    k_size_t data_size = (k_size_t)(__tls_data_end - __tls_start);
    k_size_t size = task_tls_size();

    if (data_size > size) {
        data_size = size;
    }

    memcpy(block, __tls_start, data_size);
    memset(block + data_size, 0, size - data_size);
}

static k_u64_t task_tls_base(const struct port_task *pt)
{
    if (pt == NULL || pt->tls == NULL) {
        return (k_u64_t)(uintptr_t)__tls_end;
    }

    return (k_u64_t)(uintptr_t)(pt->tls + task_tls_size());
}

/* The ABI of the machine wants every stack 16 bytes aligned. */
#define PORT_STACK_ALIGN 16u

/* The page of the processor: the guard under a stack is one of these. */
#define PORT_PAGE_SIZE 4096u

/*
 * The frame that `int $0x80` leaves on the kernel stack of a program: the ten
 * registers the stub saves and the five the processor pushed for the crossing
 * to ring 3. A fork copies it as it stands, so that the child returns from
 * the call it did not make - with the result changed to zero.
 */
#define PORT_SYSCALL_FRAME_RAX (9u * sizeof(k_u64_t))

/*
 * The frame port_x86_64_switch() pops: six callee-saved registers and the
 * address it returns to. The eighth word is padding that keeps the frame a
 * multiple of 16, so the task starts with the stack alignment the ABI
 * expects at the first instruction of a function.
 */
#define PORT_FRAME_SIZE (8u * sizeof(k_u64_t))

/*
 * The contexts of one core (ADR 0012). Every core has the context it runs,
 * the context it was before its first task - kmain() on the core 0, the idle
 * loop of an application processor - and the two slots of the switch that
 * leaves a dead task behind. The slots belong to the core and not to the
 * machine: with one core the array has one entry and this is the code of 1.0.
 */
struct port_cpu {
    struct port_task *current;
    struct port_task boot; /* the context of this core before its first task */
    struct port_task *pending_free;
    k_u64_t pending_space; /* the page tables of the task being released */
};

static struct port_cpu g_ports[KERNEL_CPUS_MAX];

/*
 * The cores whose local APIC timer did not start. A core without a clock has
 * no wake-up of its own, so an idle loop that halted there would never look
 * at its run queue again: the wait spins instead (see wait_for_interrupt()).
 * A core with a timer - the core 0 with the PIT, an AP with its APIC - halts.
 */
static bool g_timer_missing[KERNEL_CPUS_MAX];

/*
 * The tasks that began to run on a core through the interrupt of another core
 * - the wake of the scheduler, kernel/port/cpu.h - and the ones that began
 * through the core's own timer, with the tick of the core at the last wake.
 *
 * The two counters are the answer to the question the debt of the many cores
 * asks (docs/debts.md, K8): a task that a core starts in the body of the IPI
 * started before that core's timer could have offered it, and the number says
 * how often. A core that wakes its own tasks at its tick is the machine
 * before the call existed, and its counter says that too.
 */
static k_u32_t g_wake_before_tick[KERNEL_CPUS_MAX];
static k_u32_t g_wake_tick[KERNEL_CPUS_MAX];
static k_u32_t g_tick_start[KERNEL_CPUS_MAX];

/*
 * A request of another core, carried by the same vector as the wake: the
 * command `smp wake` of the shell stops the clock of a core to prove that an
 * interrupt of another core is what starts a task there, and starts it again
 * before it returns. The request is a word the core takes and clears, so a
 * command that asks twice does not leave a second stop behind.
 */
static k_u32_t g_request[KERNEL_CPUS_MAX];
static bool g_timer_off[KERNEL_CPUS_MAX];

/* The slots of the core that is asking. k_cpu_id() is a load of %gs:0. */
static struct port_cpu *this_cpu(void)
{
    k_u32_t cpu = k_cpu_id();

    return &g_ports[cpu < KERNEL_CPUS_MAX ? cpu : 0u];
}

static struct port_task *port_task_of(const struct task *t)
{
    if (t == NULL) {
        return NULL;
    }

    return (struct port_task *)t->ctx;
}

/* --- the floating point state of a context --- */

/*
 * The state a processor has after fninit, captured once by the boot and
 * copied into every context that has not run yet. The XMM registers and MXCSR
 * are not part of fninit: MXCSR starts at 0x1F80, with every exception
 * masked and the rounding to nearest, which is the state a C program expects
 * and a zeroed area would not give it (an unmasked exception would take a
 * program down at the first division by zero).
 *
 * The area is written by fxsave64 and read by fxrstor64, so the pointers of
 * the x87 instruction are kept whole and not cut at 32 bits.
 */
static k_u8_t g_fpu_clean[PORT_FPU_AREA] __attribute__((aligned(PORT_FPU_ALIGN)));
static bool g_fpu_ready;

static k_u8_t *fpu_area(struct port_task *pt)
{
    uintptr_t address = (uintptr_t)pt->fpu;

    return (k_u8_t *)((address + (PORT_FPU_ALIGN - 1u)) & ~(uintptr_t)(PORT_FPU_ALIGN - 1u));
}

/* Save the live unit into the image of this context. */
static void fpu_save(struct port_task *pt)
{
    __asm__ volatile("fxsave64 (%0)" : : "r"(fpu_area(pt)) : "memory");
}

/* Put the image of this context into the live unit. */
static void fpu_load(struct port_task *pt)
{
    __asm__ volatile("fxrstor64 (%0)" : : "r"(fpu_area(pt)) : "memory");
}

/* Give a context the state of a processor that has just executed fninit. */
static void fpu_reset(struct port_task *pt)
{
    memcpy(fpu_area(pt), g_fpu_clean, PORT_FPU_AREA);
}

/*
 * Capture the state of the clean unit. Called by the boot before the first
 * task exists, when nothing of the kernel holds a number in a register; the
 * APs read the same bytes and never run the instructions themselves.
 */
void port_x86_64_fpu_init(void)
{
    k_u32_t mxcsr = 0x1F80u;

    if (g_fpu_ready) {
        return;
    }

    __asm__ volatile("fninit" : : : "memory");
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr) : "memory");
    __asm__ volatile("fxsave64 (%0)" : : "r"(g_fpu_clean) : "memory");

    g_fpu_ready = true;

    /* The context of the core 0 is the boot: it has no task and is not made
     * by task_context_new(), but it is still a context of the switch. */
    fpu_reset(&g_ports[0].boot);
}

/*
 * Release the stack of a task that exited. It cannot be done by the task
 * itself: the block holds the stack it is running on, and the switch that
 * leaves it saves the stack pointer inside that same block. The next context
 * of this core that gets the CPU does it instead, before it runs any kernel
 * code, and the dead context is never resumed, so nothing uses the bytes
 * afterwards. The slot is the core's: each core releases the context it last
 * left behind.
 */
static void release_pending(void)
{
    struct port_cpu *self = this_cpu();
    struct port_task *dead = self->pending_free;
    k_u64_t flags;

    if (dead == NULL || dead == self->current) {
        return;
    }

    self->pending_free = NULL;
    flags = port_save_flags_and_cli();

    /*
     * The guard page under the stack is the port's, not the allocator's: it
     * is not present in the map of the kernel and the block is a run of
     * bytes the heap gives to the next task. The page goes back before the
     * bytes do, or the next allocation that lands on it would fault on a
     * page nobody promised to be missing (ADR 0048).
     */
    if (dead->guard != 0) {
        port_x86_64_paging_kernel_guard_remove(dead->guard);
        dead->guard = 0;
    }

    k_port_free(dead->block);

    if (dead->tls != NULL) {
        k_port_free(dead->tls);
        dead->tls = NULL;
    }

    port_restore_flags(flags);

    /*
     * The page tables of a program outlive the switch that leaves it: the
     * dead context stopped with its own CR3 in the register, and the frames
     * may be read until the processor is on another map. Here the map of the
     * running task is the one in the register, so the tables and the frames
     * of the dead task are only bytes the allocator may have back.
     */
    if (self->pending_space != 0) {
        port_x86_64_paging_space_destroy(self->pending_space);
        self->pending_space = 0;
    }
}

/*
 * Build the frame a task starts from: what port_x86_64_switch() will pop
 * when the task is given the CPU for the first time.
 *
 * The entry point, its argument and the flags travel in R12, R13 and R14
 * because those are the registers the switch restores from the frame and a
 * task that has never run has no other way to receive a value. The bootstrap
 * in switch.S moves them to the registers of the usual ABI and calls the
 * trampoline.
 */
static void stack_build(struct port_task *pt, k_u64_t frame_rsp, k_port_task_entry_fn entry,
                        void *arg, k_u64_t flags)
{
    k_u64_t *frame = (k_u64_t *)(uintptr_t)frame_rsp;

    frame[0] = 0;                                              /* r15 */
    frame[1] = flags;                                          /* r14: the RFLAGS of the task */
    frame[2] = (k_u64_t)(uintptr_t)arg;                        /* r13: the argument of the entry */
    frame[3] = (k_u64_t)(uintptr_t)entry;                      /* r12: the entry point */
    frame[4] = 0;                                              /* rbx */
    frame[5] = 0;                                              /* rbp */
    frame[6] = (k_u64_t)(uintptr_t)port_x86_64_task_bootstrap; /* the ret address */
    frame[7] = 0;                                              /* padding: see PORT_FRAME_SIZE */

    pt->rsp = frame_rsp;
}

/*
 * Give the CPU to another context. The caller has already chosen the task
 * and claimed it with k_sched_claim_next(): the scheduler marked it RUNNING
 * on this core under its lock, so no other core can take it between that
 * choice and this jump. Nothing of the kernel bookkeeping is left to do here;
 * the interrupts are off for the length of the switch.
 */
static void commit_switch(struct port_task *prev, struct port_task *next)
{
    this_cpu()->current = next;

    /*
     * The window of a copy into the memory of a program (ADR 0052) travels
     * with its context and not with the core. AC is a flag of the processor,
     * so the context that gives up the CPU leaves it in its own record - the
     * copy it was in the middle of is the operation a program asked for, and
     * a read or a write may sleep before the bytes travel - and the context
     * that takes the CPU gets its own answer back below. A task that is taken
     * down inside a window therefore does not leave SMAP open for the next
     * one, which is the half of this that the attack suite would miss.
     */
    prev->copying = (port_x86_64_read_flags() & PORT_X86_64_RFLAGS_AC) != 0;

    /*
     * The thread pointer follows the task, not the machine: each one has its
     * own errno, and the block is put in the register before the other
     * context is resumed with it.
     */
    port_x86_64_tls_kernel();

    /*
     * The address space travels with the context. The kernel's own map is
     * part of every space, so the switch of CR3 here is safe while the
     * processor is still on the stack of the context that gives up the CPU;
     * a program is entered with the stack named by RSP0, which is its own
     * kernel stack and not the stack of the caller.
     */
    port_x86_64_paging_switch(next->cr3 != 0 ? next->cr3 : port_x86_64_kernel_cr3());

    if (next->is_user) {
        port_x86_64_tss_set_rsp0(next->stack_top);
    }

    /*
     * The floating point state travels with the context, the way the general
     * registers and CR3 do. The kernel is built with -mgeneral-regs-only and
     * touches neither the x87 unit nor the XMM registers, so the live state
     * here is the one of the context that gives up the CPU: it is saved into
     * its image before the other one is loaded, and a program that computes
     * in a double keeps its numbers across every switch. The save is done
     * unconditionally - about a hundred cycles on a machine whose tick is a
     * millisecond and whose quantum is ten - because a lazy state (CR0.TS and
     * the #NM handler) would add a trap and a second path of the scheduler
     * for a cost nobody can see (docs/debts.md, K9).
     */
    fpu_save(prev);
    fpu_load(next);

    port_x86_64_switch(&prev->rsp, next->rsp);

    /*
     * This context is the one that runs now, and the window it left open - if
     * any - is the one it gets back. The context is read from the core and
     * not from `next`: a task may be resumed from a call of the switch whose
     * `next` was another context - `port_x86_64_yield_to_idle()` hands the
     * CPU to the boot context and is resumed by the scheduler from the same
     * call - and the flag belongs to the task that runs, not to the one that
     * was asked for a moment ago.
     */
    if (this_cpu()->current != NULL && this_cpu()->current->copying) {
        port_x86_64_user_access_begin();
    } else {
        port_x86_64_user_access_end();
    }
}

/*
 * Choose the next task and switch to it, if the choice is a different task.
 *
 * k_sched_claim_next() chooses and claims in one critical section: the task
 * it returns is already RUNNING on this core and out of the ready set, so the
 * choice cannot be repeated by another core in the window before the switch.
 * A choice equal to the running task simply means "keep going"; the task may
 * leave that queue when its quantum expires or when it sleeps or blocks,
 * which is why the scheduler and this function agree about who is in the
 * queue at every moment.
 */
static void schedule(void)
{
    struct port_cpu *self = this_cpu();
    struct port_task *prev;
    struct port_task *next_pt;
    struct task *next;
    k_u64_t flags;

    flags = port_save_flags_and_cli();

    prev = self->current;
    next = k_sched_claim_next();

    if (next == NULL) {
        /* Nothing is runnable: the caller waits for a tick or an unblock. */
        port_restore_flags(flags);
        return;
    }

    if (prev != NULL && next == prev->task) {
        /* The claim of the task that is already running here: keep going. */
        port_restore_flags(flags);
        release_pending();
        return;
    }

    next_pt = port_task_of(next);
    if (next_pt == NULL) {
        /* k_sched_claim_next() refuses a task without a context, so this is
         * a record that lost its context afterwards: stay where we are. */
        port_restore_flags(flags);
        kerror("x86_64: the task '%s' has no context", next->name);
        return;
    }

    commit_switch(prev != NULL ? prev : &self->boot, next_pt);

    /*
     * We are back in this context after one or many switches elsewhere. The
     * flags below are the ones this context saved when it gave up the CPU,
     * not the ones of the context that has just left it.
     */
    port_restore_flags(flags);
    release_pending();
}

/*
 * Give the CPU back to the context the core booted on - the idle loop of the
 * core - and return here when the scheduler gives it back.
 *
 * The boot context is not a task and is not in a run queue: the scheduler
 * reaches it only when no task is runnable, and a task that keeps itself
 * ready waiting for a lock it holds keeps the core from it for good. A mutex
 * whose owner is that context is therefore a mutex a task may wait for
 * forever, which is the deadlock a filesystem section long enough for a tick
 * made real (ADR 0047). A caller that knows whom it waits for asks for the
 * switch here: the boot context finishes its section and the scheduler
 * resumes the caller.
 */
void port_x86_64_yield_to_idle(void)
{
    struct port_cpu *self = this_cpu();
    k_u64_t flags;

    if (self->current == NULL || self->boot.rsp == 0u) {
        return;
    }

    flags = port_save_flags_and_cli();
    commit_switch(self->current, &self->boot);
    port_restore_flags(flags);
}

/*
 * Wait for the next interrupt, or spin when there is no interrupt to wait
 * for. The state of the flag is the whole difference: before the table of
 * the interrupts exists a hlt would stop the machine for good, and after it
 * a hlt is the cheapest way to pass the time until the tick.
 *
 * A core whose timer did not start is the third case: it has no clock and no
 * other wake-up, and a halt there would be a core the kernel counts and never
 * runs a task on again. It polls its queue instead - slower, but the truth.
 */
static void wait_for_interrupt(void)
{
    k_u32_t cpu = k_cpu_id();

    if (cpu < KERNEL_CPUS_MAX && g_timer_missing[cpu]) {
        __asm__ volatile("pause");
        return;
    }

    if ((port_x86_64_read_flags() & PORT_X86_64_RFLAGS_IF) != 0) {
        port_hlt();
    } else {
        __asm__ volatile("pause");
    }
}

/*
 * Keep the machine alive with nothing to run. A halted machine with the
 * interrupts on wakes up at the next tick, which looks at the queues again.
 */
static void idle_forever(void)
{
    for (;;) {
        wait_for_interrupt();
    }
}

/*
 * The allocation of one execution context: the block that carries the header
 * and the stack above it, and the copy of the TLS block of the library. Both
 * the ordinary task of the kernel and the program of a fork are made here;
 * they only differ in the frame that starts them.
 *
 * The block is laid out for the guard page of ADR 0048: the header at the
 * first address the allocator gave, one page that absorbs the alignment, the
 * guard page itself, and then the stack. The guard is a page of the identity
 * map made not present below the lowest address the stack may use, so an
 * overflow is a page fault of the kernel and not a write into the heap under
 * it. The three pages over the stack are the price of that: one page of
 * slack, the guard, and the page the header shares with the allocator's
 * block. The block is one allocation and `port_task` stays at its start, out
 * of the guard.
 */
static struct port_task *task_context_new(struct task *t, k_u8_t **block, k_u64_t *top)
{
    k_size_t size = k_port_task_stack_size();
    k_size_t total = size + 3u * PORT_PAGE_SIZE;
    struct port_task *pt;
    k_u8_t *base;
    k_u64_t guard;
    k_u64_t stack_base;

    base = (k_u8_t *)k_port_alloc(total, false);
    if (base == NULL) {
        kerror("x86_64: no memory for the stack of a task (%u bytes)", (unsigned)total);
        return NULL;
    }

    /* The block is zeroed before the guard is taken out of it: a memset of a
     * range that holds an unmapped page is the fault the guard is for. */
    memset(base, 0, total);

    /*
     * The allocator aligns its payload for its own bookkeeping, and a page of
     * the processor does not have to be that alignment. The first page of the
     * block holds the header and whatever the alignment leaves; the guard is
     * the page after it, and the stack begins on the page after the guard.
     * The header must never share a page with the guard: the page is taken
     * out of the map, and `struct port_task` - the floating point state
     * included - lives in it.
     */
    {
        uintptr_t aligned =
            ((uintptr_t)base + PORT_PAGE_SIZE - 1u) & ~(uintptr_t)(PORT_PAGE_SIZE - 1u);

        guard = (k_u64_t)aligned + PORT_PAGE_SIZE;
        stack_base = guard + PORT_PAGE_SIZE;
    }

    /* A stack that has never been used reads better as zeros in a dump; the
     * frame below the top lands on the 16 byte boundary of the ABI. */
    *top = (stack_base + size) & ~(k_u64_t)(PORT_STACK_ALIGN - 1);

    pt = (struct port_task *)base;
    pt->task = t;
    pt->block = base;
    pt->tls = NULL;
    pt->guard = 0;
    pt->stack_top = *top;
    pt->cr3 = port_x86_64_kernel_cr3();
    pt->is_user = false;

    /* A context that has never run starts with the unit after fninit. */
    fpu_reset(pt);

    if (task_tls_size() != 0) {
        pt->tls = (k_u8_t *)k_port_alloc(task_tls_size(), false);

        if (pt->tls == NULL) {
            k_port_free(base);
            kerror("x86_64: no memory for the TLS block of a task (%u bytes)",
                   (unsigned)task_tls_size());
            return NULL;
        }

        task_tls_copy(pt->tls);
    }

    if (port_x86_64_paging_kernel_guard_install(guard)) {
        pt->guard = guard;
    } else {
        kwarn("x86_64: the stack of a task has no guard page");
    }

    *block = base;
    return pt;
}

/* --- the contract of kernel/port_task.h --- */

k_size_t k_port_task_stack_size(void)
{
    /*
     * 32 KiB and not 16: the shell of this machine is also the client of
     * TLS, and one handshake of mbedTLS with a certificate chain is a few
     * kilobytes of stack deeper than a command of the filesystem. The
     * memory is here (a machine of QEMU has tens of megabytes of heap), and
     * since ADR 0048 an overflow ends the task in the guard page under the
     * stack instead of walking into the heap of the port.
     */
    return 32 * 1024;
}

int k_port_task_create(struct task *t, k_port_task_entry_fn entry, void *arg)
{
    struct port_task *pt;
    k_u8_t *base = NULL;
    k_u64_t top = 0;

    if (t == NULL) {
        return -1;
    }

    pt = task_context_new(t, &base, &top);
    if (pt == NULL) {
        return -1;
    }

    /*
     * A task inherits the interrupt state of the context that created it.
     * Before the table of the interrupts exists they are off and a task must
     * not turn them on, which would be a triple fault; once the port has
     * enabled them, every task runs with them on and gets its ticks.
     */
    pt->flags = port_x86_64_read_flags() & PORT_X86_64_RFLAGS_IF;
    stack_build(pt, top - PORT_FRAME_SIZE, entry, arg, pt->flags);

    t->ctx = pt;
    t->stack = base;
    t->stack_size = k_port_task_stack_size();

    return 0;
}

/*
 * The context of the child of a fork (K4). The kernel has made the record of
 * the task and its pid; the parent's address space has been cloned; what is
 * left is a stack that starts by returning from the system call the child did
 * not make. The frame of the parent sits at the top of its kernel stack (the
 * stub of `int $0x80` put it there and nothing above it moved); the copy goes
 * to the same place on the child's stack, and the value of RAX - the answer
 * of fork - becomes the zero of the child.
 */
int port_x86_64_task_fork_create(struct task *child, k_u64_t cr3, const void *frame,
                                 k_size_t frame_size, k_port_task_entry_fn entry)
{
    struct port_task *pt;
    k_u8_t *base = NULL;
    k_u64_t top = 0;
    k_u64_t frame_rsp;

    if (child == NULL || frame == NULL || cr3 == 0 || frame_size == 0 ||
        frame_size + PORT_FRAME_SIZE > k_port_task_stack_size()) {
        return -1;
    }

    pt = task_context_new(child, &base, &top);
    if (pt == NULL) {
        return -1;
    }

    /*
     * The child of a fork inherits the floating point state of its parent
     * (POSIX): the live unit is the parent's current state, not the image
     * that its last switch left in the area. The child is a copy of the
     * parent at this instruction, numbers included.
     */
    fpu_save(pt);

    frame_rsp = top - frame_size;
    memcpy((void *)(uintptr_t)frame_rsp, frame, frame_size);
    *(k_u64_t *)(uintptr_t)(frame_rsp + PORT_SYSCALL_FRAME_RAX) = 0;

    pt->flags = port_x86_64_read_flags() & PORT_X86_64_RFLAGS_IF;
    stack_build(pt, frame_rsp - PORT_FRAME_SIZE, entry, (void *)(uintptr_t)frame_rsp, pt->flags);

    pt->cr3 = cr3;
    pt->is_user = true;

    /*
     * The space was cloned from the parent before this stack existed, and its
     * PD may still map the whole chunk of the new stack as one large page: the
     * guard of the child would be invisible to the walk of the child's own
     * CR3. The entry of the kernel's PD is copied into the space now, before
     * the task can run (ADR 0048).
     */
    if (pt->guard != 0) {
        port_x86_64_paging_space_sync_kernel(cr3, pt->guard);
    }

    child->ctx = pt;
    child->stack = base;
    child->stack_size = k_port_task_stack_size();

    return 0;
}

k_u64_t port_x86_64_task_space(void)
{
    struct port_task *current = this_cpu()->current;

    return (current != NULL && current->is_user) ? current->cr3 : 0;
}

k_u64_t port_x86_64_task_stack_top(void)
{
    struct port_task *current = this_cpu()->current;

    return current != NULL ? current->stack_top : 0;
}

/*
 * Is `address` the guard page under the stack of the task that runs now? The
 * answer is the whole of what the fault handler needs to tell an overflow of
 * a kernel stack from a fault of the kernel: the page is one page, it belongs
 * to this context and to no other, and an address inside it can be named by
 * no code the task has a right to run (ADR 0048).
 */
bool port_x86_64_task_guard_fault(k_u64_t address)
{
    struct port_task *current = this_cpu()->current;

    if (current == NULL || current->guard == 0) {
        return false;
    }

    return address >= current->guard && address < current->guard + PORT_PAGE_SIZE;
}

/*
 * The end of a task that overflowed its kernel stack. The frame of the fault
 * is on the stack of the exception (the IST page of the core, or the top of
 * this stack when the fault could still push there), and the task is taken
 * down the way a fault of a program is: the context leaves the CPU for good,
 * its block and its address space are released by the next context, and the
 * shell that waits for it sees status 255. Nothing of the machine is stopped.
 */
void port_x86_64_task_stack_fault(k_u64_t address, k_u64_t vector, k_u64_t rip)
{
    struct task *self = k_port_task_current();

    klog(K_LOG_LEVEL_ERROR, "x86-64: the task '%s' (pid %u) reached the guard page at 0x%08x%08x",
         (self != NULL) ? self->name : "?", (self != NULL) ? (unsigned)self->pid : 0u,
         (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu));

    kinfo("x86-64: vector %u, rip 0x%08x%08x: the task ends, the machine stays", (unsigned)vector,
          (unsigned)(rip >> 32), (unsigned)(rip & 0xFFFFFFFFu));

    task_exit(255);

    /* Not reached: the exit of a task does not come back. */
    for (;;) {
    }
}

void port_x86_64_task_cow_copy(void)
{
    struct port_task *current = this_cpu()->current;

    if (current != NULL && current->copied_pages != 0xFFFFFFFFu) {
        current->copied_pages++;
    }
}

bool port_x86_64_task_is_user(void)
{
    struct port_task *current = this_cpu()->current;

    return current != NULL && current->is_user;
}

/*
 * A task that the port made as an ordinary kernel task becomes a program: the
 * space it runs in and the empty kernel stack the processor takes on its way
 * into the kernel. Called on the first run of the task, just before the iretq
 * that drops it to ring 3.
 */
void port_x86_64_task_set_space(k_u64_t cr3)
{
    struct port_task *current = this_cpu()->current;

    if (current == NULL || cr3 == 0) {
        return;
    }

    current->cr3 = cr3;
    current->is_user = true;
    port_x86_64_tss_set_rsp0(current->stack_top);

    /* The space came from the tables of this moment and already names the
     * guard of this stack, but the copy is cheap and a stale large page here
     * would cost the guard the whole point of ADR 0048. */
    if (current->guard != 0) {
        port_x86_64_paging_space_sync_kernel(cr3, current->guard);
    }

    /* The kernel is part of the new map, so the switch is safe here and the
     * iretq that follows sees the pages of the program. */
    port_x86_64_paging_switch(cr3);
}

/*
 * A new image begins with the state of a processor that has just executed
 * fninit: the numbers of the program that called execve do not survive the
 * replacement, as POSIX leaves them, and the area is reset as well so that a
 * switch before the iretq cannot bring them back. Called by port_exec.c on
 * the task that is being replaced.
 */
void port_x86_64_task_fpu_reset(void)
{
    struct port_task *current = this_cpu()->current;

    if (current == NULL) {
        /* The boot has no context to keep the image in: load the unit. */
        __asm__ volatile("fxrstor64 (%0)" : : "r"(g_fpu_clean) : "memory");
        return;
    }

    fpu_reset(current);
    fpu_load(current);
}

void k_port_task_start(struct task *t)
{
    if (t == NULL || t->ctx == NULL) {
        return;
    }

    /*
     * A task created by a running task starts at the next scheduling point:
     * the caller keeps the CPU, exactly as it does on the other ports.
     *
     * The first task of the machine is the other case: the boot context has
     * nothing above it to return to, so the CPU is handed over for good and
     * the code after task_create() does not run. This is what makes a boot
     * of the shape "create the init task, halt" work: the halt is never
     * reached.
     */
    if (this_cpu()->current == NULL) {
        schedule();
    }
}

void k_port_task_yield(void)
{
    /*
     * k_sched_yield() has already put the task at the end of its queue; a
     * caller that reaches the port directly has not. Both end up ready, so
     * the test is on the state: only a task that still believes it is
     * running needs to be queued here.
     */
    struct port_task *current = this_cpu()->current;

    if (current != NULL) {
        k_port_enter_critical();
        if (current->task != NULL && current->task->state == TASK_STATE_RUNNING) {
            k_sched_task_ready(current->task);
        }
        k_port_exit_critical();
    }

    schedule();
}

void k_port_task_stop(void)
{
    struct task *self = k_port_task_current();

    while (self != NULL && task_state_of(self) == TASK_STATE_STOPPED) {
        schedule();
        if (task_state_of(self) == TASK_STATE_STOPPED) {
            wait_for_interrupt();
        }
    }
}

/*
 * Is the sleep of the calling task over? The tick handler is what ends it,
 * by moving the task out of the sleeping state; the clock of the port is the
 * fallback for a machine whose timer is not there yet, where no tick will
 * ever arrive and the sleep still has to end after the time asked for.
 */
static bool sleep_is_over(const struct task *self, k_u32_t ticks, k_u64_t deadline_us)
{
    if (self->state != TASK_STATE_SLEEPING) {
        return true;
    }

    return k_sched_ticks() == ticks && k_port_uptime_us() >= deadline_us;
}

/*
 * A sleep of this port ends when the state of the task changes: the loop
 * below looks at it on every return from the scheduler and from the halt, and
 * a wake of the kernel changes it before the task is claimed again
 * (k_sched_wake_task_locked). The door of the host is therefore not needed
 * here - the port already answers in the time of a switch and not in the time
 * of a tick.
 */
bool k_port_task_sleep_cuttable(void)
{
    return true;
}

void k_port_task_sleep_ms(k_u32_t ms)
{
    struct port_task *current = this_cpu()->current;
    struct task *self = current != NULL ? current->task : NULL;
    k_u64_t deadline_us;
    k_u32_t ticks;

    if (ms == 0) {
        /*
         * Nothing to wait for: k_sched_sleep_ms() puts the task back to
         * "ready" as soon as this returns. A switch here would leave the
         * task sleeping and out of the run queue, and only a tick could
         * bring it back.
         */
        return;
    }

    deadline_us = k_port_uptime_us() + (k_u64_t)ms * 1000u;

    if (self == NULL) {
        /* Not in a task: there is nothing to switch to, only time to pass. */
        while (k_port_uptime_us() < deadline_us) {
            wait_for_interrupt();
        }
        return;
    }

    /*
     * k_sched_sleep_ms() has already marked the task as sleeping and given
     * the tick handler the job of waking it; this loop only waits for that
     * to happen. The task is not in the run queue while it sleeps, so the
     * switch below hands the CPU to somebody else until its turn comes back.
     */
    ticks = k_sched_ticks();

    for (;;) {
        if (sleep_is_over(self, ticks, deadline_us)) {
            return;
        }

        schedule();

        /* Woken while the CPU was away: the sleep ends here, no wait. */
        if (sleep_is_over(self, ticks, deadline_us)) {
            return;
        }

        /*
         * Nothing else was runnable: wait for the tick that wakes this task,
         * or for the clock above when there is no timer yet.
         */
        wait_for_interrupt();
    }
}

void k_port_task_exit(int code)
{
    struct port_cpu *self = this_cpu();
    struct port_task *dead = self->current;
    struct port_task *next_pt;
    struct task *next;
    k_u64_t flags;

    K_UNUSED(code);

    /*
     * The count of the pages the task copied on write after a fork goes to
     * the log now and not at every fault: a line per page would be inside the
     * output of a program that prints, and one line at the end of the task is
     * the count a person reads (ADR 0057).
     */
    if (dead != NULL && dead->copied_pages != 0u) {
        struct task *self_task = k_port_task_current();

        kinfo("x86-64: the task (pid %u) copied %u page(s) on write",
              (self_task != NULL) ? (unsigned)self_task->pid : 0u, (unsigned)dead->copied_pages);
        dead->copied_pages = 0u;
    }

    /*
     * The stack of the task cannot be released from here: the block holds
     * the stack this code is running on. The context that takes the CPU next
     * releases it (see release_pending()), and a dead context is in no run
     * queue, so this one is never resumed.
     */
    if (dead != NULL) {
        self->pending_free = dead;

        /*
         * The address space of a program is released by the next context,
         * for the same reason as its stack: the tables are the map the
         * processor is on until the switch below, and the switch cannot be
         * trusted to be the next instruction.
         */
        if (dead->is_user && dead->cr3 != 0 && dead->cr3 != port_x86_64_kernel_cr3()) {
            self->pending_space = dead->cr3;
            dead->cr3 = 0;
        }
    }

    /*
     * The tick asks this flag before it takes the CPU away: a task that has
     * been published as a zombie but has not begun its exit is still in the
     * window of the scheduler's bookkeeping, and a switch there would leave
     * a context that is in no run queue - its stack and its address space
     * would never be released. From here on the two slots above are filled,
     * so a tick may take the CPU: the context that takes it next releases
     * them, and this one is never resumed.
     */
    if (dead != NULL) {
        dead->exiting = true;
    }

    flags = port_save_flags_and_cli();
    next = k_sched_claim_next();
    next_pt = port_task_of(next);

    if (next_pt != NULL && next_pt != dead) {
        commit_switch(dead != NULL ? dead : &self->boot, next_pt);
    } else if (self->boot.rsp != 0u) {
        /*
         * Nothing else is runnable: the core goes back to the context it
         * booted on - kmain() on the core 0, the idle loop of an application
         * processor - instead of waiting here, in a context that is dead and
         * whose task slot the parent may reap at any moment. The next task of
         * this core is claimed by that context, and the context releases the
         * stack of this one on the way (release_pending).
         */
        commit_switch(dead != NULL ? dead : &self->boot, &self->boot);
    }

    port_restore_flags(flags);

    /*
     * Only a core whose boot context was never switched away from (rsp == 0)
     * reaches this: there is nothing to return to, and the dead context waits
     * for the tick that wakes somebody. The interrupts have to be on for that
     * wait: a task that ends from an exception was entered through an
     * interrupt gate, with the interrupts off.
     */
    port_sti();
    idle_forever();
}

struct task *k_port_task_current(void)
{
    struct port_task *current = this_cpu()->current;

    return current != NULL ? current->task : NULL;
}

/*
 * The first C code of a task. switch.S has moved the three values of the
 * first frame into the registers of the ABI and called this function; from
 * here on the task is an ordinary C program with its own stack.
 */
void port_x86_64_task_trampoline(k_port_task_entry_fn entry, void *arg, k_u64_t flags)
{
    /*
     * A task that has never run has no saved flags to restore, so it starts
     * with the ones captured when it was created: on when the table of the
     * interrupts exists, off when it does not.
     */
    port_restore_flags(flags);
    release_pending();

    if (entry != NULL) {
        entry(arg);
    }

    /* A task that returns from its entry point is a task that has finished. */
    k_port_task_exit(0);
}

/* --- the thread pointer --- */

/*
 * The five lines of assembly and C that make an errno belong to a task. The
 * kernel calls port_x86_64_tls_kernel() on the way into a system call and on
 * a context switch; the stub of `int $0x80` calls port_x86_64_tls_user()
 * before the iretq, because the block of the kernel is not mapped in ring 3.
 */
void port_x86_64_tls_kernel(void)
{
    port_wrmsr(0xC0000100u, task_tls_base(this_cpu()->current));
}

void port_x86_64_tls_user(void)
{
    port_wrmsr(0xC0000100u, 0);
}

/* --- the tick of the timer --- */

/*
 * The interrupt handler of the timer calls this once per millisecond. The
 * interrupts are off here: the handler is entered through an interrupt gate,
 * and the EOI is the business of the handler, not of this function.
 *
 * A switch made from here is a switch from the context of the task the timer
 * interrupted: its interrupted code, the frame of the interrupt and this
 * call are all on that stack, and the task resumes them — first the rest of
 * the handler, then its own instruction — when it is given the CPU again.
 *
 * The handler has to have sent its EOI before it calls this. The reason is
 * not the switch itself, which is safe either way, but the timer: until the
 * EOI goes out the line of the PIT is still in service, and the clock of the
 * machine stops for as long as this task waits for its next turn.
 */
void port_x86_64_task_tick(void)
{
    struct port_cpu *cpu = this_cpu();
    struct port_task *self_pt;
    struct port_task *next_pt;
    struct task *next;

    /* The clock, the sleeping tasks and the quantum of the running one. */
    k_sched_tick();

    /*
     * The context of this core is the port's own record and not the task
     * table: the parent of a zombie may reap its slot as soon as the state is
     * ZOMBIE, which the kernel publishes before the port has left the CPU,
     * and the record of the moment may then be a slot that no longer names
     * the stack under us. The port knows the context it switched to.
     */
    self_pt = cpu->current;

    /*
     * A task that left the scheduler but has not reached k_port_task_exit()
     * yet is between the kernel's bookkeeping and the port's own exit. Its
     * slot may already be gone and its stack is about to be published in the
     * pending slot; a switch here would leave a context that is in no queue.
     * It is a handful of instructions from leaving by itself, and it keeps
     * the CPU for them. The test is the kernel's record of the core, not the
     * task table: k_sched_task_terminated() clears it, and a slot that was
     * reaped keeps no state worth reading.
     */
    if (self_pt != NULL && self_pt->task != NULL && k_cpu_current() != self_pt->task) {
        return;
    }

    /*
     * A task whose quantum is not over keeps the CPU; k_sched_tick() leaves it
     * running. The quantum of a preempted task is refilled by the tick that
     * moves it to the end of its queue. The context of a core with no task -
     * the idle loop of the boot or of an application processor - has none and
     * falls through to the claim.
     */
    if (self_pt != NULL && self_pt->task != NULL && self_pt->task->state == TASK_STATE_RUNNING) {
        return;
    }

    next = k_sched_claim_next();
    next_pt = port_task_of(next);

    if (next_pt == NULL) {
        /* Nothing is runnable: this context waits for the next tick or for an
         * unblock, and the idle loop of the core looks again. */
        return;
    }

    if (next_pt == self_pt) {
        /* The choice is the context that is already here: it stays. */
        release_pending();
        return;
    }

    {
        /* The switch below is the timer of this core offering a task: the
         * other way a task starts is the body of the IPI, which counts
         * itself. */
        k_u32_t id = k_cpu_id();

        if (id < KERNEL_CPUS_MAX) {
            (void)__atomic_add_fetch(&g_tick_start[id], 1u, __ATOMIC_RELAXED);
        }
    }

    commit_switch(self_pt != NULL ? self_pt : &cpu->boot, next_pt);
    /* Resumed inside the handler: the interrupts stay off until its iretq. */
    release_pending();
}

/*
 * The other core asks this one to look at its run queue (the body of the IPI
 * vector, called with the interrupts off, ADR 0012).
 *
 * It is the short path of the tick and not a second scheduler: the claim is
 * the same call the tick makes, under the same lock, and the task it returns
 * is already RUNNING on this core when the switch leaves. A core that has
 * nothing to run - or whose running task is the one the claim names - simply
 * returns and the vector acknowledges the interrupt.
 *
 * The EOI belongs to the vector body: this function must not send it, because
 * the switch below may not come back for a long time and the line has to be
 * free for the next IPI.
 */
void port_x86_64_task_ipi(void)
{
    struct port_cpu *self = this_cpu();
    struct port_task *next_pt;
    struct task *next;
    k_u32_t id = k_cpu_id();
    k_u32_t request = 0u;

    /*
     * A request of the other core first: the clock of this core is the one
     * thing the experiment of `smp wake` changes, and it changes before the
     * claim below so that a stopped clock is already stopped when the task of
     * the experiment is claimed.
     */
    if (id < KERNEL_CPUS_MAX) {
        request = __atomic_exchange_n(&g_request[id], 0u, __ATOMIC_ACQ_REL);
    }

    if ((request & PORT_X86_64_REQUEST_TIMER_OFF) != 0u && id < KERNEL_CPUS_MAX) {
        port_x86_64_apic_stop_timer();
        __atomic_store_n(&g_timer_off[id], true, __ATOMIC_RELEASE);
    }

    if ((request & PORT_X86_64_REQUEST_TIMER_ON) != 0u && id < KERNEL_CPUS_MAX) {
        bool started = port_x86_64_apic_start_timer(1000u);

        __atomic_store_n(&g_timer_off[id], !started, __ATOMIC_RELEASE);
    }

    /*
     * The window the tick refuses and this body has to refuse too: a task that
     * has left the scheduler - k_sched_task_terminated() cleared the record of
     * the core - but has not reached k_port_task_exit() yet. Its stack and its
     * address space are published to the next context of the core a handful of
     * instructions later, and a switch here would abandon the context that was
     * about to publish them: nothing would ever release the block, and a
     * machine of many cores would lose 32 KiB of the heap per task that the
     * interrupt caught in that window (the leak the 600-second session found).
     */
    if (self->current != NULL && self->current->task != NULL &&
        k_cpu_current() != self->current->task) {
        return;
    }

    next = k_sched_claim_next();

    if (next == NULL) {
        return;
    }

    if (self->current != NULL && next == self->current->task) {
        /* The claim of the task that already runs here: nothing to do. */
        return;
    }

    next_pt = port_task_of(next);
    if (next_pt == NULL) {
        kerror("x86_64: the task '%s' has no context", next->name);
        return;
    }

    if (id < KERNEL_CPUS_MAX) {
        /* The task starts in the body of the interrupt of the other core, and
         * that is before this core's next timer tick: the number is the wake
         * before the tick of the debt of the many cores, and the tick beside
         * it is the moment of the core when it happened. */
        (void)__atomic_add_fetch(&g_wake_before_tick[id], 1u, __ATOMIC_RELAXED);
        g_wake_tick[id] = k_sched_cpu_ticks(id);
    }

    commit_switch(self->current != NULL ? self->current : &self->boot, next_pt);
    /* Resumed inside the handler: the interrupts stay off until its iretq. */
    release_pending();
}

/* --- what the `smp` command reads about the wake of a core --- */

k_u32_t port_x86_64_wake_before_tick(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        return 0u;
    }

    return __atomic_load_n(&g_wake_before_tick[cpu], __ATOMIC_RELAXED);
}

k_u32_t port_x86_64_wake_tick(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        return 0u;
    }

    return __atomic_load_n(&g_wake_tick[cpu], __ATOMIC_RELAXED);
}

k_u32_t port_x86_64_tick_start(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        return 0u;
    }

    return __atomic_load_n(&g_tick_start[cpu], __ATOMIC_RELAXED);
}

/*
 * Ask a core to stop or to start the clock of its own local APIC. The request
 * rides on the vector of the wake: the command of the shell that wants to
 * prove that an interrupt starts a task there stops the clock first, because
 * a core with a timer would pick the task at its next tick and the two paths
 * would be indistinguishable. A core that is not there takes nothing, and the
 * answer of port_x86_64_timer_off() is the flag the core itself wrote.
 */
void port_x86_64_timer_off_request(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX || cpu == k_port_cpu_id()) {
        return;
    }

    (void)__atomic_fetch_or(&g_request[cpu], PORT_X86_64_REQUEST_TIMER_OFF, __ATOMIC_ACQ_REL);
    port_x86_64_apic_send_ipi(port_x86_64_cpu_apic_id(cpu), PORT_X86_64_VECTOR_APIC_IPI);
}

void port_x86_64_timer_on_request(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX || cpu == k_port_cpu_id()) {
        return;
    }

    (void)__atomic_fetch_or(&g_request[cpu], PORT_X86_64_REQUEST_TIMER_ON, __ATOMIC_ACQ_REL);
    port_x86_64_apic_send_ipi(port_x86_64_cpu_apic_id(cpu), PORT_X86_64_VECTOR_APIC_IPI);
}

bool port_x86_64_timer_off(k_u32_t cpu)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        return false;
    }

    return __atomic_load_n(&g_timer_off[cpu], __ATOMIC_ACQUIRE);
}

/* --- the context of an application processor --- */

/*
 * Prepare the boot context of a core that has just reached C. The idle loop
 * of the core is the code the first switch saves there, and it is the context
 * that comes back when the core has no task to run. Called by the core itself
 * (src/smp.c), before the first switch; `stack_top` is the top of the stack
 * the trampoline put the core on.
 */
void port_x86_64_ap_sched_init(k_u32_t cpu, k_u64_t stack_top)
{
    struct port_cpu *self;

    if (cpu >= KERNEL_CPUS_MAX) {
        return;
    }

    self = &g_ports[cpu];
    memset(&self->boot, 0, sizeof(self->boot));
    self->boot.task = NULL;
    self->boot.stack_top = stack_top;
    self->boot.cr3 = port_x86_64_kernel_cr3();
    self->boot.is_user = false;
    fpu_reset(&self->boot);
    self->current = NULL;
    self->pending_free = NULL;
    self->pending_space = 0;

    /*
     * The thread pointer of this core before its first switch: the kernel's
     * own TLS block, because the idle context has no task. A processor that
     * has just left the reset has zero in IA32_FS_BASE, and the first
     * _Thread_local of the kernel would be a load from address zero.
     */
    port_x86_64_tls_kernel();
}

/*
 * What an application processor does when it has nothing to run: look at its
 * queue, and wait for the interrupt that brings it work. The interrupts are
 * on - that is the rule of a context of this port, and this is the context of
 * the core itself - so the wait of wait_for_interrupt() is the halt that the
 * local APIC timer, an IPI or the next task ends.
 */
void port_x86_64_idle(void)
{
    for (;;) {
        schedule();
        wait_for_interrupt();
    }
}

void port_x86_64_ap_idle(void)
{
    port_x86_64_idle();
}

/*
 * Record whether the local APIC timer of a core started. The bring-up of an
 * application processor calls this before it enters the idle loop; a core
 * without a clock cannot halt (see wait_for_interrupt()), so this is the one
 * bit that keeps a core the kernel counts from stopping for good.
 */
void port_x86_64_ap_timer_ready(k_u32_t cpu, bool ready)
{
    if (cpu >= KERNEL_CPUS_MAX) {
        return;
    }

    g_timer_missing[cpu] = !ready;
}
