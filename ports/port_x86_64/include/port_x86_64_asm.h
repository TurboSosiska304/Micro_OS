/*
 * Micro OS — the numbers the C of the x86-64 port and its assembly share.
 *
 * A file of assembly is preprocessed, so it can read a header, but it cannot
 * read a header that names a type of the kernel: `k_u64_t` is a word of a C
 * compiler, and the assembler has no idea what it is. This header is therefore
 * nothing but preprocessor definitions - no types, no declarations, no
 * includes - so that src/interrupts.S and the C of the port can agree about
 * the offsets of the record of a core without one of them repeating a number
 * the other may change.
 *
 * The record itself is a struct in src/cpu.c, which asserts every number here
 * against offsetof(): a field that moves and a header that does not is a build
 * that stops and not a call that lands on the stack of nothing.
 */
#ifndef MICRO_OS_PORT_X86_64_ASM_H
#define MICRO_OS_PORT_X86_64_ASM_H

/*
 * The words of the record of a core, at the address IA32_GS_BASE of the core
 * points at. The kernel number is the first one (k_port_cpu_id() is a load of
 * offset zero and nothing else); the two below it are what the entry of the
 * fast door of a system call reads before there is a C frame to ask.
 *
 * KERNEL_RSP  the stack the processor takes on a call - the same number as
 *             RSP0 of the TSS, written by port_x86_64_tss_set_rsp0(), which is
 *             the one place every entry into ring 3 goes through;
 * USER_RSP    the stack of the program at the instruction `syscall`. The
 *             instruction does not save it and the frame of the call needs it,
 *             so the entry keeps it here for the four instructions it is
 *             worth. It is a slot of the core: one core runs one program.
 */
#define PORT_X86_64_CPU_ID 0
#define PORT_X86_64_CPU_APIC_ID 4
#define PORT_X86_64_CPU_KERNEL_RSP 8
#define PORT_X86_64_CPU_USER_RSP 16

/*
 * The two ring 3 selectors of a frame, as the port writes them into a frame
 * and as the tail of a call checks them: the code and the data descriptor of
 * ring 3 of the GDT of src/gdt.c, with the ring in the low two bits. The
 * numbers are the ones a frame of port_exec.c carries as well.
 */
#define PORT_X86_64_USER_CS 0x1b
#define PORT_X86_64_USER_SS 0x23

#endif
