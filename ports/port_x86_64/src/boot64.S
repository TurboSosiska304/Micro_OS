/*
 * Micro OS — the first instruction of the 64-bit kernel.
 *
 * The loader of the port (loader32.S, or the UEFI application of
 * loader/uefi) has already switched the processor to long mode, copied this
 * image to its physical address and mapped it at the top of the address
 * space, and jumped here with two arguments: the information of the boot in
 * RDI and, in RSI, the magic of the hand-off when the boot was a UEFI machine
 * (loader/uefi) - the loader of the BIOS path leaves RSI zero. The entry is a
 * high address of the image (ADR 0007): the stack below is high, the clear of
 * .bss writes high, and the boot information stays the physical address the
 * loader left in RDI, because the low identity map is still the one the
 * devices and the memory map are read through. From this point the kernel
 * runs the way it does on a board: the same C sources, the same boot
 * sequence.
 */
.section .text.boot, "ax", @progbits
.global _start64
.type _start64, @function

_start64:
    cli

    /* The stack is in .bss of this image, and the image is linked high. */
    movabs $stack_top, %rsp
    mov %rdi, %r12 /* the boot information; rep stosb will use %rdi */
    mov %rsi, %r13 /* which of the two boots it is */

    /* The image was copied as bytes: the .bss has to be cleared here. */
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    xor %eax, %eax
    cld
    rep stosb

    mov %r12, %rdi
    mov %r13, %rsi
    xor %ebp, %ebp
    call kmain

1:  cli
    hlt
    jmp 1b

.size _start64, . - _start64

.section .bss
.align 16
stack_bottom:
.skip 65536
stack_top:

.section .note.GNU-stack, "", @progbits
