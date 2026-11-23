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

/* The steps of the search of a free destination for the copy of the kernel:
 * the first block of 2 MiB at or above KERNEL_PHYS (16 MiB), and the last one
 * whose image still ends below the arena of the programs at 32 MiB. */
.set KERNEL_STEP, 0x00200000
.set KERNEL_LAST, 0x01E00000

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

.extern loader_verify_image
.extern loader_puts

_start:
    cli
    cld

    /* The copy of the trampoline below is a string move, and a multiboot
       loader is not required to leave ES where this code wants it. */
    xorw %ax, %ax
    movw %ax, %es

    /* Zero our .bss first: the tables and the stack live there. */
    lea __bss_start, %edi
    lea __bss_end, %ecx
    subl %edi, %ecx
    xorl %eax, %eax
    rep stosb

    /* The map of the memory is in EBX; it lives below 1 GiB and stays
       reachable through the identity map. */
    movl %ebx, mbi_ptr

    /*
     * The C of the signature check runs before the page tables exist, so its
     * stack is the one in .bss and every address is still the physical one
     * of the boot. loader_verify_image() prints the reason and returns a
     * failure when the image did not pass; the loader then halts, and no
     * byte of the kernel has run.
     */
    movl $stack_top, %esp

    movl mbi_ptr, %eax
    movl 20(%eax), %ecx /* mods_count */
    testl %ecx, %ecx
    jz halt
    movl 24(%eax), %edx /* mods_addr */
    testl %edx, %edx
    jz halt
    movl (%edx), %esi /* mod_start */
    movl 4(%edx), %edi /* mod_end */
    subl %esi, %edi
    pushl %edi
    pushl %esi
    call loader_verify_image
    addl $8, %esp
    testl %eax, %eax
    jnz halt

    /*
     * Where the kernel is copied: the first block of KERNEL_STEP bytes at or
     * above KERNEL_PHYS that no module of the boot lies over.
     *
     * The modules live where the loader of the machine put them, and the
     * loaders of the world disagree: QEMU and GRUB keep them below 2 MiB, and
     * limine puts them right after the kernel file - which, with a root image
     * of 16 MB, runs from 1 MiB to 18 MiB and through the address this loader
     * has always used for the copy. A module under the copy is a boot this
     * loader cannot take (the copy would destroy the root image of the medium
     * exactly when the kernel is about to read it), so the destination moves
     * up until it is free; a boot whose modules leave no block below the arena
     * of the programs is refused here, before a byte moves.
     */
    movl mbi_ptr, %eax
    movl 20(%eax), %ebx /* mods_count */
    movl 24(%eax), %ebp /* mods_addr */
    movl (%ebp), %edx /* mod_start of the kernel */
    movl 4(%ebp), %edi
    subl %edx, %edi /* the bytes of the image */

    movl $KERNEL_PHYS, %esi /* the candidate destination */
.Ldest_next:
    xorl %ecx, %ecx /* the index of the module */
.Ldest_check:
    cmpl %ebx, %ecx
    jae .Ldest_ok /* no module of the boot is in the way */
    movl %ecx, %eax
    shll $4, %eax
    addl %ebp, %eax /* &mods[%ecx] */
    leal (%esi, %edi), %edx /* dest_end */
    cmpl %esi, 4(%eax)
    jbe .Ldest_next_module /* the module ends before the destination */
    cmpl %edx, (%eax)
    jae .Ldest_next_module /* the module starts after it */
    addl $KERNEL_STEP, %esi
    cmpl $KERNEL_LAST, %esi
    jbe .Ldest_next
    pushl $nowhere_text
    call loader_puts
    addl $4, %esp
    jmp halt
.Ldest_next_module:
    incl %ecx
    jmp .Ldest_check

.Ldest_ok:
    movl %esi, dest_phys

    /*
     * The trampoline of the application processors (src/ap_trampoline.S) is
     * linked at the physical page the start-up IPI names, 0x7000, and this
     * loader is the only code that runs before the APs do: it puts the bytes
     * there while the addresses are still the physical ones. The kernel then
     * fills the parameter block at 0x7F00 and sends the IPIs.
     */
    movl $0x7000, %edi
    movl $ap_trampoline_blob, %esi
    movl $(ap_trampoline_blob_end - ap_trampoline_blob), %ecx
    rep movsb

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
    addl dest_phys, %eax
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
    movl dest_phys, %edi
    cld
    rep movsb

    /* PAE, LME, then paging: the order the processor requires. The two other
       bits are the floating point of a program: OSFXSR lets ring 3 execute
       the SSE instructions and FXSAVE/FXRSTOR work at all, and OSXMMEXCPT
       makes an SSE fault an exception of the machine. The x87 unit needs no
       bit of CR4; CR0.NE below reports its errors as an exception and not as
       an external interrupt line.

       The kernel itself is built with -mgeneral-regs-only and touches
       neither unit; the scheduler saves the state of a task with FXSAVE and
       loads it with FXRSTOR (src/port_x86_64_task.c), so two programs may
       compute in floating point at once. The application processors get the
       same bits in ap_trampoline.S. */
    movl %cr4, %eax
    orl $0x620, %eax
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
    /* PG | PE | NE: the paging, the protected mode and the numeric error of
       the x87 unit as an exception (CR0.NE) rather than the external line of
       a machine that no longer has a coprocessor. */
    orl $0x80000021, %eax
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
       reads it through the identity map; the entry itself is at the top. The
       second argument of the entry is the magic of the hand-off of a UEFI
       machine (loader/uefi): this loader has none, and the port reads the
       zero as "the multiboot information of the BIOS path". The third is
       where the bytes of the kernel went, which is not always KERNEL_PHYS. */
    movl mbi_ptr(%rip), %edi
    xorl %esi, %esi
    movl dest_phys(%rip), %edx
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

/* Where the copy of the kernel is, decided above: KERNEL_PHYS on a boot whose
 * modules are out of the way, and higher when one of them is not. */
dest_phys:
.long KERNEL_PHYS

.section .rodata
.align 8
nowhere_text:
.asciz "Micro OS: no free block below the arena of the programs for the kernel image\r\n"

/*
 * The flat image of the AP trampoline, linked at 0x7000 by ap_trampoline.ld
 * and copied to that address above. The bytes carry their own absolute
 * addresses, so they are data here and not code the loader runs.
 */
.align 16
ap_trampoline_blob:
.incbin "ap_trampoline.bin"
ap_trampoline_blob_end:

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
