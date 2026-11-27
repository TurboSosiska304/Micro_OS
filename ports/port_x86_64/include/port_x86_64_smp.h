/*
 * Micro OS — the x86-64 port: the cores, between the two files that own them.
 *
 * src/cpu.c reads the processors of the firmware, gives every one of them a
 * kernel number and answers which core is asking; src/smp.c starts the ones
 * the boot does not run on. Neither half is a part of the contract of
 * kernel/port/cpu.h, so the few calls they share live here instead of in
 * port_x86_64.h, which is the header of the machine for the kernel.
 *
 * The identity of a core is a record of this port, not a register of the
 * processor: the number the kernel reads on every scheduling decision is the
 * first word of that record, and IA32_GS_BASE points at it (see the top of
 * src/cpu.c). The hardware identifier from the MADT and CPUID is a different
 * number, and it is the one an INIT/SIPI has to name.
 */
#ifndef MICRO_OS_PORT_X86_64_SMP_H
#define MICRO_OS_PORT_X86_64_SMP_H

#include "kernel/types.h"

/* The hardware identifier (the local APIC id) of the core with that kernel
 * number, 0 for a number the machine does not have. The two are not the same
 * thing: QEMU gives a machine of two processors the ids 0 and 1, a machine of
 * many sockets does not. */
k_u32_t port_x86_64_cpu_apic_id(k_u32_t cpu);

/*
 * The words of the record of a core, at the offsets src/interrupts.S reads.
 * They live in a header of preprocessor definitions only, because the file of
 * assembly cannot include a header of the kernel's types (see
 * port_x86_64_asm.h, which says what the four are and who writes them).
 */
#include "port_x86_64_asm.h"

/*
 * The stack of the program that is running, for the entry of `syscall`. Called
 * by port_x86_64_tss_set_rsp0() with the value the TSS was just given, so that
 * the record cannot fall behind the descriptor of the processor.
 */
void port_x86_64_cpu_set_kernel_rsp(k_u64_t rsp);

/*
 * Make `cpu` the identity of the caller. The processor of the boot calls this
 * from port_x86_64_cpus_init(); an application processor calls it as its
 * first action in C, before any kernel call, because a call may ask which
 * core it is. It points IA32_GS_BASE at the record of the core and opens the
 * read of k_port_cpu_id(). A port built without KERNEL_SMP does nothing here
 * and every core is the core 0.
 */
void port_x86_64_cpu_set_self(k_u32_t cpu);

/* What the bring-up knows about one core. The bits are a record of the boot,
 * not a promise: a core without APIC or TIMER reached C and runs, but its
 * clock or its interrupts are what the machine did not give it. */
#define PORT_X86_64_CORE_REACHED 0x1u /* it executed C of this kernel */
#define PORT_X86_64_CORE_ONLINE 0x2u  /* the boot accepted it into the kernel */
#define PORT_X86_64_CORE_APIC 0x4u    /* its local APIC is enabled */
#define PORT_X86_64_CORE_TIMER 0x8u   /* its local APIC timer ticks */

/* The record of one core, 0 for a number the machine does not have. */
k_u32_t port_x86_64_smp_core_status(k_u32_t cpu);

/* The cores the firmware listed that did not answer the start-up IPI. It is
 * kept for the boot to name; the count the kernel uses is the one of the
 * cores that really reached C (port_x86_64_smp_checked_in). */
k_u32_t port_x86_64_smp_failures(void);

/* Put the `smp` command in the shell: the cores of the machine, a run of
 * workers on them and the IPI between two of them. 0 when it is registered. */
int port_x86_64_smp_register(void);

/*
 * The first C of an application processor (src/smp.c). The trampoline of the
 * port jumps here with the number of the core in RDI; it never returns.
 */
[[noreturn]] void port_x86_64_ap_entry(k_u32_t cpu);

#endif /* MICRO_OS_PORT_X86_64_SMP_H */
