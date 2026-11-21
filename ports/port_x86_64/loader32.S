/*
 * Micro OS — the loader of the x86-64 port, the first instruction.
 *
 * QEMU loads a multiboot 1 image at one megabyte and starts it in 32-bit
 * protected mode. This file is the whole of the bootloader the port has:
 * it copies the 64-bit kernel (a multiboot module, a flat binary built for
 * 16 MiB) where it wants to be, builds the identity map of the first
 * gigabyte, turns the long mode on and jumps into the kernel with the map
 * of the memory of the boot.
 *
 * There is no configuration file and no protocol to negotiate: the boot is
 * one page of assembly that the project owns, the way Linux has its own.
 */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000003 /* align the modules, ask for the memory map */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)
.set KERNEL_ADDR, 0x01000000

.section .multiboot, "a", @progbits
.align 8
.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM

.section .text
.code32
.global _start
.type _start, @function

_start:
    cli
    cld

    /* Zero our .bss first: the tables and the stack live there. */
    lea __bss_start, %edi
    lea __bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    /* The map of the memory is in EBX; it lives below 1 GiB and stays
       reachable through the identity map. */
    movl %ebx, mbi_ptr

    /* PML4[0] -> PDPT, PDPT[0] -> PD: the first gigabyte through 2 MiB
       pages, so that every address of the boot keeps its meaning. */
    movl $pdpt, %eax
    orl $0x03, %eax
    movl %eax, pml4
    movl $pd, %eax
    orl $0x03, %eax
    movl %eax, pdpt

    xorl %ecx, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, pd(, %ecx, 8)
    movl $0, pd + 4(, %ecx, 8)
    incl %ecx
    cmpl $512, %ecx
    jb 1b

    /*
     * The kernel comes as the first module of the boot: QEMU puts it in the
     * memory and describes it in the multiboot information. It is copied to
     * its link address before the switch, while the addresses are still the
     * physical ones of the loader.
     */
    movl mbi_ptr, %eax
    movl 24(%eax), %eax /* mods_addr */
    testl %eax, %eax
    jz halt
    movl (%eax), %esi /* mod_start */
    movl 4(%eax), %ecx
    subl %esi, %ecx /* mod_end - mod_start */
    movl $KERNEL_ADDR, %edi
    cld
    rep movsb

    /* PAE, LME, then paging: the order the processor requires. */
    movl %cr4, %eax
    orl $0x20, %eax
    movl %eax, %cr4

    movl $pml4, %eax
    movl %eax, %cr3

    /* Long mode and the no-execute bit of a page: EFER.NXE (bit 11) has to
       be on before any table names a page with bit 63, because a bit the
       processor has not been told to read is a reserved bit and a fault. The
       map of the loader itself is still the identity of the boot; the kernel
       rebuilds it in src/paging.c with the text read-only and every data
       page NX (ADR 0007). */
    movl $0xC0000080, %ecx
    rdmsr
    orl $0x900, %eax
    wrmsr

    movl %cr0, %eax
    orl $0x80000001, %eax
    movl %eax, %cr0

    lgdt gdt32_ptr
    ljmp $0x08, $long_mode

.code64
long_mode:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    movl mbi_ptr(%rip), %edi
    movabs $KERNEL_ADDR, %rax
    jmp *%rax

.code32
halt:
    cli
    hlt
    jmp halt

.size _start, . - _start

.section .data
.align 8
mbi_ptr:
.long 0

.section .rodata
.align 8
gdt64:
.quad 0
.quad 0x00AF9A000000FFFF /* code, 64-bit, present, ring 0 */
.quad 0x00CF92000000FFFF /* data, present, ring 0 */

/* In 32-bit mode the processor reads six bytes of the pointer. */
gdt32_ptr:
.word gdt64_end - gdt64 - 1
.long gdt64
gdt64_end:

.section .bss
.align 4096
pml4:
.skip 4096
pdpt:
.skip 4096
pd:
.skip 4096

.align 16
.stack:
.skip 16384
stack_top:

.section .note.GNU-stack, "", @progbits
