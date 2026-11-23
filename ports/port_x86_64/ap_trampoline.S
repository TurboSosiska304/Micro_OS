/*
 * Micro OS — the trampoline that brings an application processor up.
 *
 * A second processor of the x86-64 machine does not start where the kernel
 * is: after INIT and the start-up IPI it wakes in real mode at the physical
 * page named by the vector of the IPI, with CS pointing at that page and no
 * page tables, no GDT of the kernel and no stack. This file is the whole of
 * the way from there into C, at a fixed physical address that the loader
 * copies it to (0x7000, the page the SIPI vector 7 names).
 *
 * The code is linked at that address, so its absolute references - the GDT
 * pointer, the descriptors, the parameter block - are already the physical
 * numbers the AP needs. The BSP writes the parameters before the IPI: the
 * page tables of the kernel, the top of the stack of this core, the address
 * of the C entry and the number of the core. Nothing here allocates, prints
 * or reads a table of the firmware: the AP is alone until it reaches C.
 *
 * The stages are the ones the processor requires of a switch to long mode:
 * real mode -> protected mode with a flat 32-bit table -> PAE and EFER.LME ->
 * CR0.PG -> a far jump to a 64-bit code segment -> the C entry.
 *
 * The parameter block sits on the next page, 0x8000, and has four 64-bit
 * words:
 *
 *   0x8000  the physical address of the kernel PML4 (CR3)
 *   0x8008  the top of the stack of this core (16 byte aligned)
 *   0x8010  the address of port_x86_64_ap_entry() in the higher half
 *   0x8018  the number of this core
 *
 * The two pages are split for the W^X rule of the kernel map (ADR 0007): the
 * code page 0x7000 is read-only and executable, the parameter page 0x8000 is
 * writable and not executable. The AP executes the first after it turns the
 * paging on, so the first must not carry the NX bit, and the BSP writes the
 * second before every IPI, so the second must stay writable.
 */
.section .text, "ax", @progbits
.code16
.global ap_start
.type ap_start, @function
ap_start:
    cli
    cld

    /*
     * The segments of a processor that has just been started are not a
     * promise: a CS of the SIPI page yes, but DS and the others may hold the
     * leftovers of the reset. The stage below addresses memory through DS
     * with base zero, which is what the flat real mode of this machine is.
     */
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* A20 through the fast gate of the chipset. A machine that already has it
     * open reads the same byte back; the bit is idempotent. */
    inb $0x92, %al
    orb $0x02, %al
    outb %al, $0x92

    /*
     * The temporary GDT. The operand-size prefix makes the processor read the
     * 32-bit base of the pointer, which the 16-bit form would cut at 24 bits;
     * the address is 0x7DF0 with DS zero.
     */
    .byte 0x66
    lgdt ap_gdt_ptr

    movl %cr0, %eax
    orl $0x00000001, %eax /* CR0.PE */
    movl %eax, %cr0

    /* A 32-bit offset and a 32-bit code selector: the table is flat here. */
    ljmpl $0x08, $ap_pm32
.size ap_start, . - ap_start

.code32
ap_pm32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* PAE is the shape of the tables the kernel keeps. OSFXSR and
       OSXMMEXCPT are the floating point of a program: without them the SSE
       instructions of ring 3 are an invalid opcode and FXSAVE itself faults,
       so the state of a task could not be saved on this core at all. The
       processor of the boot gets the same two bits in loader32.S. */
    movl %cr4, %eax
    orl $0x00000620, %eax /* CR4.PAE | CR4.OSFXSR | CR4.OSXMMEXCPT */
    movl %eax, %cr4

    /*
     * EFER.LME and EFER.NXE. NXE has to be on before a table names a page
     * with bit 63, because a bit the processor was not told to read is a
     * reserved bit and a fault - and every data page of this kernel is NX.
     */
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x00000900, %eax
    wrmsr

    /* The kernel PML4 is below 4 GiB and CR3 is a 32-bit register here. */
    movl 0x8000, %eax
    movl %eax, %cr3

    movl %cr0, %eax
    /* CR0.PG | CR0.PE | CR0.NE: the numeric error of the x87 unit is an
       exception of the machine on this core too, as it is on the core 0. */
    orl $0x80000021, %eax
    movl %eax, %cr0

    /* The far jump loads CS with L=1 and the processor is in long mode. */
    ljmpl $0x18, $ap_lm64

.code64
ap_lm64:
    movw $0x20, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs

    /*
     * The stack of this core, then the C entry with the number of the core in
     * RDI as the ABI wants it. The slot is 16 byte aligned, so the call leaves
     * the stack the way a call is entered (RSP = 8 mod 16).
     */
    movabsq $0x8008, %rax
    movq (%rax), %rsp

    movabsq $0x8018, %rax
    movq (%rax), %rdi

    movabsq $0x8010, %rax
    movq (%rax), %rax
    call *%rax

    /* C never returns; a return here is a bug and the core stops. */
1:
    cli
    hlt
    jmp 1b

/*
 * The temporary table: a null, a 32-bit code and data pair, a 64-bit code
 * segment and a flat data segment. The selectors are 0x08, 0x10, 0x18 and
 * 0x20; the kernel builds its own table with the same numbers for the parts
 * of the machine that need ring 3, and reloads it in C.
 */
. = 0x0DF0
ap_gdt_ptr:
    .word 0x0027
    .long 0x00007E00

. = 0x0E00
ap_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF /* 32-bit code, ring 0, flat 4 GiB */
    .quad 0x00CF92000000FFFF /* 32-bit data, ring 0 */
    .quad 0x00AF9A000000FFFF /* 64-bit code, ring 0 */
    .quad 0x00CF92000000FFFF /* data, ring 0 */

/* The parameters the BSP fills in before the start-up IPI (see the top). */
. = 0x1000
ap_params:
    .quad 0x0000000000000000 /* CR3 */
    .quad 0x0000000000000000 /* stack top */
    .quad 0x0000000000000000 /* C entry */
    .quad 0x0000000000000000 /* core number */

.section .note.GNU-stack, "", @progbits
