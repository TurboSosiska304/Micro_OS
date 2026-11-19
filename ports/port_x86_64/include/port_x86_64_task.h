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
 * The C side of the first instruction of a task. The arguments arrive in the
 * registers the frame carries, not from a caller; the RFLAGS are applied
 * here, because the interrupts of a new task were never saved on its stack.
 */
void port_x86_64_task_trampoline(k_port_task_entry_fn entry, void *arg, k_u64_t flags);

/* The interrupt flag of RFLAGS. */
#define PORT_X86_64_RFLAGS_IF 0x200u

/* The flags of the machine right now, without changing them. */
static inline k_u64_t port_x86_64_read_flags(void)
{
    k_u64_t flags;

    __asm__ volatile("pushfq\n\tpopq %0" : "=r"(flags));
    return flags;
}

#endif /* MICRO_OS_PORT_X86_64_TASK_H */
