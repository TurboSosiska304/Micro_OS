/*
 * Micro OS — the first instruction of the 64-bit kernel.
 *
 * The loader of the port (loader32.S) has already switched the processor to
 * long mode, copied this image to its address and jumped here with the map
 * of the memory of the boot in RDI. From this point the kernel runs the way
 * it does on a board: the same C sources, the same boot sequence.
 */
.section .text.boot, "ax", @progbits
.global _start64
.type _start64, @function

_start64:
    cli

    /* The stack is in .bss of this image. */
    movabs $stack_top, %rsp
    mov %rdi, %r12 /* the boot information; rep stosb will use %rdi */

    /* The image was copied as bytes: the .bss has to be cleared here. */
    lea __bss_start(%rip), %rdi
    lea __bss_end(%rip), %rcx
    sub %rdi, %rcx
    xor %eax, %eax
    cld
    rep stosb

    mov %r12, %rdi
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
