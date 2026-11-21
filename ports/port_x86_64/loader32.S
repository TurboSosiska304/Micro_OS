/*
 * Micro OS — the loader of the x86-64 port, the first instruction.
 *
 * QEMU loads a multiboot 1 image at one megabyte and starts it in 32-bit
 * protected mode. This file is the whole of the bootloader the port has:
 * it copies the 64-bit kernel (a multiboot module, a flat binary built for
 * the top of the address space) where it wants to be, builds the map of the
 * boot - the identity of the first gigabyte and the kernel at the top - turns
 * the long mode on and jumps into the kernel with the map of the memory of
 * the boot.
 *
 * There is no configuration file and no protocol to negotiate: the boot is
 * one page of assembly that the project owns, the way Linux has its own.
 */
.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000003 /* align the modules, ask for the memory map */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

/*
 * The two numbers of the link: the image is linked at KERNEL_BASE (the top
 * 2 GiB, the region the small model of the compiler can name) and these
 * bytes are copied to KERNEL_PHYS, the 16 MiB the loader owns before there
 * is a heap (ADR 0007). linker64.ld and port_x86_64.h carry the same pair.
 */
.set KERNEL_BASE, 0xFFFFFFFF80000000
.set KERNEL_PHYS, 0x01000000

/*
 * The high map covers the image and its bss in 2 MiB pages: 16 MiB, which is
 * the allowance of src/paging.c (KERNEL_IMAGE_TABLES) and far more than the
 * image of the port. The kernel rebuilds the same chunks with 4 KiB pages at
 * the boot; this map only has to carry the clear of .bss and the first C.
 */
.set KERNEL_MAP_2MIB, 8

/* The index of PML4 entry 511 and of PDPT entry 510: the address
 * 0xFFFFFFFF80000000 is the base of the top 2 GiB, and 0xFFFF...BC00 is the
 * end of the second gigabyte of the 512 GiB the entry 511 names. */
.set PML4_HIGH_OFFSET, 511 * 8
.set PDPT_HIGH_OFFSET, 510 * 8

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

    /* PML4[0] -> PDPT_LOW, PDPT_LOW[0] -> PD_LOW: the first gigabyte through
       2 MiB pages, so that every address of the boot keeps its meaning. */
    movl $pdpt_low, %eax
    orl $0x03, %eax
    movl %eax, pml4
    movl $pd_low, %eax
    orl $0x03, %eax
    movl %eax, pdpt_low

    xorl %ecx, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, pd_low(, %ecx, 8)
    movl $0, pd_low + 4(, %ecx, 8)
    incl %ecx
    cmpl $512, %ecx
    jb 1b

    /*
     * PML4[511] -> PDPT_HIGH, PDPT_HIGH[510] -> PD_HIGH: the same physical
     * KERNEL_PHYS and up, named at KERNEL_BASE. A PDPT entry covers 1 GiB,
     * so entry 510 of the PML4 entry 511 is exactly the top 2 GiB. The two
     * halves get a PDPT each: the high map is supervisor-only and a program
     * inherits it as it is, while the low one is copied per address space.
     */
    movl $pdpt_high, %eax
    orl $0x03, %eax
    movl %eax, pml4 + PML4_HIGH_OFFSET
    movl $0, pml4 + PML4_HIGH_OFFSET + 4
    movl $pd_high, %eax
    orl $0x03, %eax
    movl %eax, pdpt_high + PDPT_HIGH_OFFSET
    movl $0, pdpt_high + PDPT_HIGH_OFFSET + 4

    xorl %ecx, %ecx
2:
    movl %ecx, %eax
    shll $21, %eax
    addl $KERNEL_PHYS, %eax
    orl $0x83, %eax
    movl %eax, pd_high(, %ecx, 8)
    movl $0, pd_high + 4(, %ecx, 8)
    incl %ecx
    cmpl $KERNEL_MAP_2MIB, %ecx
    jb 2b

    /*
     * The kernel comes as the first module of the boot: QEMU puts it in the
     * memory and describes it in the multiboot information. It is copied to
     * its physical address before the switch, while the addresses are still
     * the physical ones of the loader.
     */
    movl mbi_ptr, %eax
    movl 24(%eax), %eax /* mods_addr */
    testl %eax, %eax
    jz halt
    movl (%eax), %esi /* mod_start */
    movl 4(%eax), %ecx
    subl %esi, %ecx /* mod_end - mod_start */
    movl $KERNEL_PHYS, %edi
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

    /* The boot information is a physical address below 1 GiB and the kernel
       reads it through the identity map; the entry itself is at the top. */
    movl mbi_ptr(%rip), %edi
    movabs $KERNEL_BASE, %rax
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
pdpt_low:
.skip 4096
pd_low:
.skip 4096
pdpt_high:
.skip 4096
pd_high:
.skip 4096

.align 16
.stack:
.skip 16384
stack_top:

.section .note.GNU-stack, "", @progbits
