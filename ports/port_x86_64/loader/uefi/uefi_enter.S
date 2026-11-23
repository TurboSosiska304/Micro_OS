/*
 * Micro OS — the last step of the UEFI loader: the long mode's paging on, the
 * kernel's GDT, and the jump.
 *
 * The firmware has already left this program in the 64-bit mode, with paging
 * of its own, an identity map of the memory and a GDT of its own. What the
 * kernel of this port expects is one thing more: it is linked at
 * PORT_X86_64_KERNEL_BASE, the top 2 GiB, and its bytes are somewhere below
 * (the loader says where), so the map of the boot has to name the image at the
 * top as well as the identity the firmware left.
 *
 * The tables are built by the C of this loader (`build_tables`, the three
 * numbers of a 2 MiB page are easier to read there than here) and this file
 * does the four registers of the processor in the order it requires: PAE, the
 * address of the tables, the long mode with the no-execute bit, the paging.
 *
 * The jump is the whole of the loader's contract with the kernel, and it is
 * the one of the BIOS path with one argument more: RDI is the boot
 * information and RSI the magic that says which boot it is (the loader of the
 * BIOS path leaves RSI zero). The entry is at KERNEL_BASE (src/boot64.S).
 */
.set KERNEL_BASE, 0xFFFFFFFF80000000

.text
.code64
.globl port_x86_64_enter_kernel

/* void port_x86_64_enter_kernel(u64 stack, u64 boot_info, u64 magic, u64 pml4);
 *
 * The caller is the C of this loader, and the loader is compiled for the
 * target of the firmware - the x86-64 Windows ABI, which is the convention of
 * every call of the firmware and of this one: RCX, RDX, R8 and R9. The jump
 * does not come back. */
port_x86_64_enter_kernel:
    cli
    cld

    /* The three values of the jump and the tables, kept across the work
     * below: the registers of this code are not the caller's any more. */
    mov %rdx, %r12 /* boot_info */
    mov %r8, %r13  /* magic */
    mov %r9, %r14  /* the PML4 of the boot */

    /* The stack the loader allocated before it left the boot services: the
     * one the firmware gave this program is memory of the boot. */
    mov %rcx, %rsp
    xor %rbp, %rbp

    /* PAE, and the two bits of the floating point of a program: the kernel
     * sets them itself as well, and a processor told to execute SSE without
     * them faults on the first instruction of a program that does. */
    mov %cr4, %rax
    or $0x620, %eax
    mov %rax, %cr4

    mov %r14, %cr3

    /* Long mode and the no-execute bit of a page (EFER.NXE): the kernel's
     * tables mark the data with bit 63, and a bit the processor has not been
     * told to read is a reserved bit and a fault. */
    mov $0xC0000080, %ecx
    rdmsr
    or $0x900, %eax /* LME | NXE */
    wrmsr

    mov %cr0, %rax
    or $0x80000021, %eax /* PG | PE | NE */
    mov %rax, %cr0

    lgdt gdt_pointer(%rip)

    /* The code segment of this GDT: the selector is reloaded by a far return,
     * because a far jump with a 64-bit offset has no immediate form. */
    lea .Llong_mode(%rip), %rax
    push $0x08
    push %rax
    lretq

.Llong_mode:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs

    mov %r12, %rdi
    mov %r13, %rsi
    mov $0xFFFFFFFF80000000, %rax
    jmp *%rax

.data
.p2align 4
gdt:
    .quad 0
    .quad 0x00AF9A000000FFFF /* code, 64-bit, present, ring 0 */
    .quad 0x00CF92000000FFFF /* data, present, ring 0 */

gdt_pointer:
    .word gdt_end - gdt - 1
    .quad gdt
gdt_end:
