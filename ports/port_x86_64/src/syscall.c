/*
 * Micro OS — the fast door of a program on x86-64.
 *
 * A call used to be one instruction for the program and a whole gate of the
 * processor under it: `int $0x80` reads a descriptor of the IDT, checks the
 * privilege of it, loads the stack of ring 0 from the TSS, pushes five words
 * and starts the stub of the port. The machine has a pair of instructions
 * that do the crossing in the registers of the core, and this file turns them
 * on: `syscall` jumps to the address of IA32_LSTAR with the flags of
 * IA32_FMASK cleared, the return address of the program in RCX and its flags
 * in R11, and `sysret` goes back through the pair of selectors that
 * IA32_STAR names.
 *
 * What the instruction does not do is change the stack, and the whole point
 * of the crossing here is that a program never runs in ring 0 on the stack it
 * chose. The stub of src/interrupts.S takes the stack of the program from the
 * record of the core, which is what the descriptor of the IDT used to do
 * through RSP0 of the TSS; gdt.c writes the same number to both places, so the
 * two doors land on the same stack.
 *
 * The state is one register of the core, and a core that is started later has
 * one of each as well: the boot opens the door for itself at the end of
 * port_x86_64_interrupts_init() and an application processor on its way up
 * (src/smp.c). A core that never opened it takes a call the old way - the
 * vector 0x80 is still there, and it is still what a signal handler returns
 * through (port_exec.c), so a machine of two doors is a machine where the
 * slow one is a fallback and not a ruin.
 *
 * The vector stays for one more reason: the frame the two doors build is the
 * same 160 bytes in the same order (PORT_X86_64_SYSCALL_FRAME_SIZE), which is
 * what lets the signal frame, the child of a fork and the return of an exec
 * be written once for both.
 */
#include "port_x86_64.h"

/* The registers of the crossing (Intel SDM 4, "Model-Specific Registers"). */
#define MSR_EFER 0xC0000080u  /* extended features: SCE is bit 0 */
#define MSR_STAR 0xC0000081u  /* the selectors of both instructions */
#define MSR_LSTAR 0xC0000082u /* the entry of `syscall` */
#define MSR_FMASK 0xC0000084u /* the flags `syscall` clears */

/* SCE: the instruction `syscall` is enabled in 64-bit mode. */
#define EFER_SCE 0x1u

/* The flags of the processor, by the names the manual gives them (the bits of
 * RFLAGS the mask below is made of). */
#define FLAG_TF 0x100u   /* trap: one instruction, then an exception */
#define FLAG_IF 0x200u   /* interrupts */
#define FLAG_DF 0x400u   /* the direction of a string instruction */
#define FLAG_NT 0x4000u  /* a nested task */
#define FLAG_AC 0x40000u /* alignment check */

/*
 * The flags the processor clears on the way in, before the first instruction
 * of the stub runs. IF is the one that matters: the frame of the call is not
 * on the stack yet, and an interrupt that arrived there would push a frame of
 * its own over the stack of the program, which is not a stack of the kernel.
 * The stub turns the interrupts on itself once the frame is safe, exactly as
 * the gate did.
 *
 * DF is cleared because the kernel is compiled for the ABI, where a string
 * instruction counts up; the AC flag because the kernel of ring 0 must not
 * fault on an unaligned access a program made legal for itself; TF and NT
 * because a trap or a nested task of ring 3 means nothing in ring 0, and the
 * stub returns the flags of the program from the frame anyway.
 */
#define SYSCALL_FMASK (FLAG_DF | FLAG_IF | FLAG_TF | FLAG_AC | FLAG_NT)

/* STAR: SS of the call is the descriptor above CS, and SS of the return is
 * the descriptor below CS (src/gdt.c). */
#define SYSCALL_STAR                                                                               \
    (((k_u64_t)PORT_X86_64_SYSRET_STAR << 48) | ((k_u64_t)PORT_X86_64_SYSCALL_CS << 32))

/*
 * Open the door of this core. The order is the one of the manual: SCE first,
 * because an entry written into LSTAR means nothing until the instruction
 * exists, and FMASK last, so that the first call of this core already finds
 * the mask of the flags it was written for.
 */
void port_x86_64_syscall_init_cpu(void)
{
    k_u64_t efer = port_rdmsr(MSR_EFER);

    if ((efer & EFER_SCE) == 0u) {
        port_wrmsr(MSR_EFER, efer | EFER_SCE);
    }

    port_wrmsr(MSR_STAR, SYSCALL_STAR);
    port_wrmsr(MSR_LSTAR, (k_u64_t)(uintptr_t)isr_syscall_fast);
    port_wrmsr(MSR_FMASK, SYSCALL_FMASK);
}
