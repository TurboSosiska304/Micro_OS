/*
 * Micro OS — the x86-64 port: the pieces of the task substrate that the C
 * and the assembly halves share.
 *
 * kernel/port_task.h is the contract with the kernel: a stack, a way to
 * create and to start a task, to yield, to sleep and to exit. This header is
 * the other half of the same subject — the switch itself and the frame a
 * task starts from — and it is internal to the port: nothing in the kernel
 * includes it.
 */
#ifndef MICRO_OS_PORT_X86_64_TASK_H
#define MICRO_OS_PORT_X86_64_TASK_H

#include "kernel/port_task.h"
#include "kernel/types.h"

/*
 * Save the context of the caller and continue in another one. Defined in
 * switch.S; the layout of the frame it builds is described there and in
 * port_x86_64_task.c, which builds the first frame of a task by hand.
 */
void port_x86_64_switch(k_u64_t *old_rsp, k_u64_t new_rsp);

/* The first instruction of a task: it is the address the switch returns to. */
void port_x86_64_task_bootstrap(void);

/*
 * A task whose program was forked: its kernel stack already carries the frame
 * of the system call of the parent, with the result of fork set to zero, and
 * its first return goes to ring 3 through it. `frame` is the parent's frame
 * (the fifteen saved registers and the iretq frame, 160 bytes), copied below
 * the top of the new stack. `entry` runs in ring 0 first and never returns.
 */
int port_x86_64_task_fork_create(struct task *child, k_u64_t cr3, const void *frame,
                                 k_size_t frame_size, k_port_task_entry_fn entry);

/*
 * The address space of the program that runs now, zero when the caller is a
 * kernel task; the top of its kernel stack (RSP0 of the TSS); and the call
 * that makes the current context a program with a space of its own.
 */
k_u64_t port_x86_64_task_space(void);
k_u64_t port_x86_64_task_stack_top(void);
void port_x86_64_task_set_space(k_u64_t cr3);

/*
 * The guard page under the stack of a kernel task (ADR 0048). `guard_fault`
 * is true when the address belongs to the page under the stack of the context
 * that runs now; `stack_fault` says so on COM1 and in the log and ends that
 * task with status 255, on the stack of the exception and without ever
 * returning. The caller is port_x86_64_exception(), which has already read
 * CR2 and checked that the fault came from ring 0.
 */
bool port_x86_64_task_guard_fault(k_u64_t address);
void port_x86_64_task_stack_fault(k_u64_t address, k_u64_t vector, k_u64_t rip)
    __attribute__((noreturn));

/* Is the task that runs now a program of ring 3? */
bool port_x86_64_task_is_user(void);

/*
 * The C side of the first instruction of a task. The arguments arrive in the
 * registers the frame carries, not from a caller; the RFLAGS are applied
 * here, because the interrupts of a new task were never saved on its stack.
 */
void port_x86_64_task_trampoline(k_port_task_entry_fn entry, void *arg, k_u64_t flags);

/*
 * The floating point state of the tasks (docs/debts.md, K9). The boot
 * captures the state of a unit after fninit once, before the first task; the
 * switch saves and loads the 512-byte image of every context, a fork copies
 * the live state of the parent into the child, and an exec resets the live
 * state of the task it replaces. The board has neither the units nor the
 * switch, so nothing of this is part of the kernel's contract.
 */
void port_x86_64_fpu_init(void);
void port_x86_64_task_fpu_reset(void);

/* --- the context of a core that is not the processor of the boot --- */

/*
 * Prepare the boot context of an application processor: the context its first
 * switch saves, and the one that comes back when the core has no task to run.
 * Called by the core itself on its way up (src/smp.c), before the first
 * switch, with the top of the stack the trampoline gave it.
 */
void port_x86_64_ap_sched_init(k_u32_t cpu, k_u64_t stack_top);

/*
 * The idle loop of an application processor: look at the queue, then wait for
 * the interrupt that brings work. It is the code the boot context of the core
 * resumes into, and it never returns; the interrupts are on in it, because
 * the timer of the core is what ends the wait.
 */
void port_x86_64_ap_idle(void) __attribute__((noreturn));

/*
 * Say whether the local APIC timer of a core started. A core without a clock
 * has no wake-up, so its idle loop polls the queue instead of halting on it:
 * the kernel counts that core and must still be able to run a task there.
 */
void port_x86_64_ap_timer_ready(k_u32_t cpu, bool ready);

/*
 * The idle loop of a core: look at the run queue and wait for the interrupt
 * that brings it work. The boot context of the core 0 (kmain after the first
 * task was started) and the context of an application processor are both this
 * loop, and it is what the port returns to when the last task of a core ends.
 */
void port_x86_64_idle(void) __attribute__((noreturn));

/*
 * Give the CPU to the boot context of the core (the idle loop of the core 0)
 * and return when the scheduler resumes the caller. The boot context is not
 * in a run queue, so a task that waits for a lock it holds has to ask.
 */
void port_x86_64_yield_to_idle(void);

/* The interrupt flag of RFLAGS. */
#define PORT_X86_64_RFLAGS_IF 0x200u

/*
 * The frame that an entry from ring 3 leaves on the kernel stack of a
 * program: the fifteen registers the stub saves and the five the processor
 * pushed for the crossing to ring 3. The call and the tick of the timer save
 * the same fifteen registers in the same order, so a frame has one shape
 * wherever it came from (see port_exec.c, user_frame()). A fork copies it as
 * it stands, so that the child returns from the call it did not make with
 * every register the parent had - the callee-saved five included, because a
 * child does not come back through the call of the parent and cannot rely on
 * a C compiler to keep them.
 */
#define PORT_X86_64_SYSCALL_FRAME_SIZE 160u

/* The flags of the machine right now, without changing them. */
static inline k_u64_t port_x86_64_read_flags(void)
{
    k_u64_t flags;

    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags));
    return flags;
}

#endif /* MICRO_OS_PORT_X86_64_TASK_H */
