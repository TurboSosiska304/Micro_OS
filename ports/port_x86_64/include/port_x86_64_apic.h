/*
 * Micro OS — the x86-64 port: the local and I/O APICs.
 *
 * The 8259 of the port delivers the timer and the keyboard to the processor
 * of the boot, and it does so through the external interrupt pin of that
 * processor's local APIC. The other cores have no 8259: their interrupts are
 * the local APIC timer, the inter-processor interrupts and, once the I/O
 * APIC is programmed, the lines the chip routes to them (ADR 0012).
 *
 * This header is the contract between the two halves of that work: the
 * bring-up of the processors (src/smp.c, src/cpu.c) asks for an identifier,
 * a timer and an INIT/SIPI, and the APIC code (src/lapic.c, src/ipi.c and,
 * later, src/ioapic.c) owns the registers. Nothing here sleeps: every call is
 * a store to the page of the APIC or a short wait on the TSC.
 *
 * The page is mapped where the kernel map has room, in the top 2 GiB, and
 * every address space shares that mapping, so a core finds its APIC whichever
 * CR3 the scheduler put in the register.
 */
#ifndef MICRO_OS_PORT_X86_64_APIC_H
#define MICRO_OS_PORT_X86_64_APIC_H

#include "kernel/types.h"
#include "port_x86_64.h"

/*
 * The window of the local APIC in the kernel's map: 2 MiB aligned, above the
 * screen of the port (CONSOLE and PORT_X86_64_SCREEN_VIRT) and below the end
 * of the mapped top 2 GiB. The physical page arrives from the MADT; a machine
 * whose firmware names another address gets it mapped here all the same.
 */
#define PORT_X86_64_APIC_VIRT (PORT_X86_64_KERNEL_BASE + 0x30000000ull)

/*
 * The vectors of the APIC. They sit above the window of the 8259 (0x20..0x2F)
 * and below the door of the programs (0x80), so a stub of interrupts.S and
 * the table of the boot keep one numbering for every core.
 */
#define PORT_X86_64_VECTOR_APIC_TIMER 0x50u
#define PORT_X86_64_VECTOR_APIC_IPI 0x51u
#define PORT_X86_64_VECTOR_APIC_SPURIOUS 0xFFu

/*
 * Map the page of the APIC, enable it on the core that calls this and give
 * the spurious vector its entry. The processor of the boot calls it once the
 * page tables exist; an application processor calls it on its way up. The
 * answer is false when the firmware named no APIC or the page cannot be
 * mapped, in which case no other call of this header may be used.
 *
 * The external pin of the local APIC (LINT0) is left in the ExtINT mode the
 * firmware programmed: that is the way the 8259 reaches the processor of the
 * boot, and a bring-up that changes it would lose the timer and the keyboard
 * of the machine in the same store.
 */
bool port_x86_64_apic_init(void);

/* True when the APIC of this build is up. */
bool port_x86_64_apic_present(void);

/* The hardware identifier of the core that asks, 0 when the APIC is not up.
 * The identifiers are not the numbers of the kernel: QEMU gives a machine of
 * two processors the ids 0 and 1, a machine of many sockets does not. */
k_u32_t port_x86_64_apic_id(void);

/* Acknowledge the interrupt in service. Called by the body of a vector the
 * APIC raised, after the work of the handler. */
void port_x86_64_apic_eoi(void);

/*
 * Start the periodic timer of this core: `hz` interrupts a second, one per
 * millisecond for the tick of the kernel. The divider of the APIC is
 * calibrated against the TSC of this core, because the frequency of the
 * counter is not a number the architecture promises. Returns false when the
 * measurement gave nothing (the timer is then left stopped).
 */
bool port_x86_64_apic_start_timer(k_u32_t hz);

/* Stop the timer of this core (mask its vector). */
void port_x86_64_apic_stop_timer(void);

/* Send an IPI of `vector` to the core with that hardware id. */
void port_x86_64_apic_send_ipi(k_u32_t apic_id, k_u32_t vector);

/*
 * The start of an application processor: INIT, then the start-up IPI with
 * the page of the trampoline (vector 7 names 0x7000). The caller waits
 * between the two and sends the start-up IPI twice, which is the sequence of
 * the PC firmware and the one QEMU answers. Returns false when the delivery
 * of a command did not finish in the timeout: a core that did not take the
 * command is a core that has to be reported, not counted.
 */
bool port_x86_64_apic_send_init(k_u32_t apic_id, k_u32_t timeout_us);
bool port_x86_64_apic_send_startup(k_u32_t apic_id, k_u32_t vector, k_u32_t timeout_us);

/* --- the bodies of the vectors, called by interrupts.S --- */

/* The timer of one core: the tick of the kernel, then the scheduler. */
void port_x86_64_irq_lapic_timer(k_u64_t *frame);

/* An inter-processor interrupt: the other core asks this one to look at its
 * run queue. The frame is the one of the interrupted code. */
void port_x86_64_irq_ipi(k_u64_t *frame);

/* A spurious interrupt has no state and no EOI: the APIC already answered. */
void port_x86_64_irq_spurious(k_u64_t *frame);

/* The number of IPIs this core has taken since the boot (a number the shell
 * and the tests read; it is how a run proves the path is not dead code). */
k_u32_t port_x86_64_ipi_received(void);

/* The same number for the core with that kernel number, 0 when there is no
 * such core. A command of the shell sends an IPI to the other core, waits a
 * moment and reads this: the two numbers are the proof that the vector of
 * the interrupt ran on the other processor and not in a comment. */
k_u32_t port_x86_64_ipi_received_cpu(k_u32_t cpu);

#endif /* MICRO_OS_PORT_X86_64_APIC_H */
