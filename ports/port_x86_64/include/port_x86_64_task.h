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

/* Is the task that runs now a program of ring 3? */
bool port_x86_64_task_is_user(void);

/*
 * The C side of the first instruction of a task. The arguments arrive in the
 * registers the frame carries, not from a caller; the RFLAGS are applied
 * here, because the interrupts of a new task were never saved on its stack.
 */
void port_x86_64_task_trampoline(k_port_task_entry_fn entry, void *arg, k_u64_t flags);

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
