/*
 * Micro OS — the long jump of a program, in the registers of x86-64.
 *
 * The C standard puts the shape of `jmp_buf` in the machine's hands, and the
 * machine's answer is the six callee-saved registers of the System V ABI
 * (rbx, rbp, r12, r13, r14, r15), the stack pointer and the address the call
 * returns to. A caller-saved register does not have to survive: the compiler
 * has already spilled what it needs across the call of `setjmp`, which the
 * declaration marks `returns_twice`.
 *
 * The stack pointer of the saved frame is the one *after* the return of
 * `setjmp`, so the jump lands in the caller as if the call had returned a
 * second time, with the value of `longjmp` as its answer (1 when the caller
 * passed 0, as the standard says).
 *
 * Only a build of x86-64 compiles this file: the board has no long jump of
 * this library, because no program of the board links one.
 */
#include "ulibc.h"

#include <setjmp.h>

#if !defined(__x86_64__)
#error "the long jump of the library of a program is written for x86-64"
#endif

int setjmp(jmp_buf environment)
{
    __asm__ volatile("movq %%rbx, 0(%0)\n\t"
                     "movq %%rbp, 8(%0)\n\t"
                     "movq %%r12, 16(%0)\n\t"
                     "movq %%r13, 24(%0)\n\t"
                     "movq %%r14, 32(%0)\n\t"
                     "movq %%r15, 40(%0)\n\t"
                     "leaq 8(%%rsp), %%r11\n\t"
                     "movq %%r11, 48(%0)\n\t"
                     "movq (%%rsp), %%r11\n\t"
                     "movq %%r11, 56(%0)"
                     :
                     : "r"(environment)
                     : "r11", "memory");

    return 0;
}

void longjmp(jmp_buf environment, int value)
{
    __asm__ volatile("movl %1, %%eax\n\t"
                     "testl %%eax, %%eax\n\t"
                     "jne 1f\n\t"
                     "incl %%eax\n\t"
                     "1:\n\t"
                     "movq %0, %%r11\n\t"
                     "movq 0(%%r11), %%rbx\n\t"
                     "movq 8(%%r11), %%rbp\n\t"
                     "movq 16(%%r11), %%r12\n\t"
                     "movq 24(%%r11), %%r13\n\t"
                     "movq 32(%%r11), %%r14\n\t"
                     "movq 40(%%r11), %%r15\n\t"
                     "movq 56(%%r11), %%rdx\n\t"
                     "movq 48(%%r11), %%rsp\n\t"
                     "jmp *%%rdx"
                     :
                     : "r"(environment), "r"(value)
                     : "rax", "rbx", "rbp", "rdx", "r11", "r12", "r13", "r14", "r15", "memory");

    __builtin_unreachable();
}
