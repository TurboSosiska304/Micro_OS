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

#include "kernel/log.h"
#include "kernel/port/lock.h"
#include "kernel/port/mem.h"
#include "kernel/port/time.h"
#include "kernel/port_task.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "port_x86_64.h"

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
    k_u64_t cr3;       /* the page tables of the task; the kernel's by default */
    bool is_user;      /* a program of ring 3 runs on this context */
    bool exiting;      /* k_port_task_exit has begun on this context */
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

static struct port_task *g_current;
static struct port_task g_boot; /* the context of kmain, before the first task */
static struct port_task *g_pending_free;
static k_u64_t g_pending_space; /* the page tables of the task being released */

static struct port_task *port_task_of(const struct task *t)
{
    if (t == NULL) {
        return NULL;
    }

    return (struct port_task *)t->ctx;
}

/*
 * Release the stack of a task that exited. It cannot be done by the task
 * itself: the block holds the stack it is running on, and the switch that
 * leaves it saves the stack pointer inside that same block. The next context
 * that gets the CPU does it instead, before it runs any kernel code, and the
 * dead context is never resumed, so nothing uses the bytes afterwards.
 */
static void release_pending(void)
{
    struct port_task *dead = g_pending_free;
    k_u64_t flags;

    if (dead == NULL || dead == g_current) {
        return;
    }

    g_pending_free = NULL;
    flags = port_save_flags_and_cli();
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
    if (g_pending_space != 0) {
        port_x86_64_paging_space_destroy(g_pending_space);
        g_pending_space = 0;
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
    g_current = next;

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

    port_x86_64_switch(&prev->rsp, next->rsp);
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
    struct port_task *prev;
    struct port_task *next_pt;
    struct task *next;
    k_u64_t flags;

    flags = port_save_flags_and_cli();

    prev = g_current;
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

    commit_switch(prev != NULL ? prev : &g_boot, next_pt);

    /*
     * We are back in this context after one or many switches elsewhere. The
     * flags below are the ones this context saved when it gave up the CPU,
     * not the ones of the context that has just left it.
     */
    port_restore_flags(flags);
    release_pending();
}

/*
 * Wait for the next interrupt, or spin when there is no interrupt to wait
 * for. The state of the flag is the whole difference: before the table of
 * the interrupts exists a hlt would stop the machine for good, and after it
 * a hlt is the cheapest way to pass the time until the tick.
 */
static void wait_for_interrupt(void)
{
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
 */
static struct port_task *task_context_new(struct task *t, k_u8_t **block, k_u64_t *top)
{
    k_size_t size = k_port_task_stack_size();
    struct port_task *pt;
    k_u8_t *base;

    base = (k_u8_t *)k_port_alloc(size, false);
    if (base == NULL) {
        kerror("x86_64: no memory for the stack of a task (%u bytes)", (unsigned)size);
        return NULL;
    }

    /*
     * The allocator aligns its payload for its own bookkeeping, which is not
     * the alignment of the ABI; the stack is cut from the top of the block
     * downwards so that the frame below lands on a 16 byte boundary.
     */
    *top = (k_u64_t)(uintptr_t)(base + size) & ~(k_u64_t)(PORT_STACK_ALIGN - 1);

    /* A stack that has never been used reads better as zeros in a dump. */
    memset(base, 0, size);

    pt = (struct port_task *)base;
    pt->task = t;
    pt->block = base;
    pt->tls = NULL;
    pt->stack_top = *top;
    pt->cr3 = port_x86_64_kernel_cr3();
    pt->is_user = false;

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
     * memory is here (a machine of QEMU has tens of megabytes of heap) and
     * a stack overflow in ring 0 is a machine that stops without a word.
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

    frame_rsp = top - frame_size;
    memcpy((void *)(uintptr_t)frame_rsp, frame, frame_size);
    *(k_u64_t *)(uintptr_t)(frame_rsp + PORT_SYSCALL_FRAME_RAX) = 0;

    pt->flags = port_x86_64_read_flags() & PORT_X86_64_RFLAGS_IF;
    stack_build(pt, frame_rsp - PORT_FRAME_SIZE, entry, (void *)(uintptr_t)frame_rsp, pt->flags);

    pt->cr3 = cr3;
    pt->is_user = true;

    child->ctx = pt;
    child->stack = base;
    child->stack_size = k_port_task_stack_size();

    return 0;
}

k_u64_t port_x86_64_task_space(void)
{
    return (g_current != NULL && g_current->is_user) ? g_current->cr3 : 0;
}

k_u64_t port_x86_64_task_stack_top(void)
{
    return g_current != NULL ? g_current->stack_top : 0;
}

bool port_x86_64_task_is_user(void)
{
    return g_current != NULL && g_current->is_user;
}

/*
 * A task that the port made as an ordinary kernel task becomes a program: the
 * space it runs in and the empty kernel stack the processor takes on its way
 * into the kernel. Called on the first run of the task, just before the iretq
 * that drops it to ring 3.
 */
void port_x86_64_task_set_space(k_u64_t cr3)
{
    if (g_current == NULL || cr3 == 0) {
        return;
    }

    g_current->cr3 = cr3;
    g_current->is_user = true;
    port_x86_64_tss_set_rsp0(g_current->stack_top);

    /* The kernel is part of the new map, so the switch is safe here and the
     * iretq that follows sees the pages of the program. */
    port_x86_64_paging_switch(cr3);
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
    if (g_current == NULL) {
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
    if (g_current != NULL) {
        k_port_enter_critical();
        if (g_current->task != NULL && g_current->task->state == TASK_STATE_RUNNING) {
            k_sched_task_ready(g_current->task);
        }
        k_port_exit_critical();
    }

    schedule();
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

void k_port_task_sleep_ms(k_u32_t ms)
{
    struct task *self = g_current != NULL ? g_current->task : NULL;
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
    struct port_task *dead = g_current;
    struct port_task *next_pt;
    struct task *next;
    k_u64_t flags;

    K_UNUSED(code);

    /*
     * The stack of the task cannot be released from here: the block holds
     * the stack this code is running on. The context that takes the CPU next
     * releases it (see release_pending()), and a dead context is in no run
     * queue, so this one is never resumed.
     */
    if (dead != NULL) {
        g_pending_free = dead;

        /*
         * The address space of a program is released by the next context,
         * for the same reason as its stack: the tables are the map the
         * processor is on until the switch below, and the switch cannot be
         * trusted to be the next instruction.
         */
        if (dead->is_user && dead->cr3 != 0 && dead->cr3 != port_x86_64_kernel_cr3()) {
            g_pending_space = dead->cr3;
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
        commit_switch(dead != NULL ? dead : &g_boot, next_pt);
    }

    port_restore_flags(flags);

    /*
     * Nothing else was runnable. This context is dead but it is still the one
     * the processor runs, so it waits for the tick that wakes somebody - and
     * the tick has to be able to arrive. A task that ends from an exception
     * was entered through an interrupt gate, with the interrupts off, and a
     * halt that waits for an interrupt that is masked waits forever.
     */
    port_sti();
    idle_forever();
}

struct task *k_port_task_current(void)
{
    return g_current != NULL ? g_current->task : NULL;
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
    port_wrmsr(0xC0000100u, task_tls_base(g_current));
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
    struct task *self;
    struct port_task *self_pt;
    struct port_task *next_pt;

    /* The clock, the sleeping tasks and the quantum of the running one. */
    k_sched_tick();

    self = k_port_task_current();
    if (self == NULL) {
        /* The boot context: there is no task to take the CPU away from. */
        return;
    }

    /*
     * A task published as a zombie is between the scheduler's bookkeeping
     * and the port's own exit (k_port_task_exit), which is what releases its
     * stack and the page tables of its program. The interrupts come back on
     * on that way - the console line of the exit is written with them on -
     * so a tick can land in the window. A switch here would leave a context
     * that is in no run queue: its release would never happen and every
     * program of the machine would leak an address space. The task is about
     * to leave by itself, and the tick keeps it on the CPU for those few
     * instructions. A task that has begun the exit has already put its stack
     * and its map in the pending slots, so it may be preempted; that is the
     * case of the last context of the machine, which waits in the idle loop
     * for the tick that wakes the parent.
     */
    if (self->state == TASK_STATE_ZOMBIE && g_current != NULL && !g_current->exiting) {
        return;
    }

    /*
     * A task whose quantum is not over keeps the CPU; k_sched_tick() leaves
     * it running. The quantum of a preempted task is refilled by the tick
     * that moves it to the end of its queue.
     */
    if (self->state == TASK_STATE_RUNNING) {
        return;
    }

    next_pt = port_task_of(k_sched_claim_next());

    if (next_pt == NULL) {
        /*
         * Nothing is runnable: the task keeps whatever it is doing - it may
         * have blocked itself - and the next unblock or tick looks again.
         */
        return;
    }

    self_pt = port_task_of(self);
    if (self_pt == NULL) {
        kerror("x86_64: the task '%s' has no context", self->name);
        return;
    }

    if (next_pt == self_pt) {
        /* The choice is the task itself: it was and stays the one to run. */
        release_pending();
        return;
    }

    commit_switch(self_pt, next_pt);
    /* Resumed inside the handler: the interrupts stay off until its iretq. */
    release_pending();
}
