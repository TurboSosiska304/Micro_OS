/*
 * Micro OS — mmu_execstack, a program that tries to run its own stack.
 *
 * The stack is data: writable and NX (ADR 0007 §9, and ADR 0052 for the
 * policy). The program puts the byte of a `ret` into a local array and jumps
 * to it. The fetch of that page raises a page fault with the bit of the
 * instruction fetch (0x15: present, user, fetch), and the port ends the task
 * with EFAULT and the address in the log.
 *
 * The array is `volatile` and its address is taken: a compiler may not keep
 * it in a register or throw it away, and the page it lives in is the stack of
 * the program and not its bss. The line after the call is the control: a
 * machine that ran the stack prints it, and the smoke test must not find it.
 */
#include "prog.h"

#define MMU_STACK_CODE 8

int mmu_execstack_main(int argc, char *const argv[])
{
    volatile unsigned char code[MMU_STACK_CODE];
    void (*body)(void);

    (void)argc;
    (void)argv;

    code[0] = 0xC3u; /* ret: the shortest program there is */
    code[1] = 0x90u;

    body = (void (*)(void))(unsigned long)(unsigned long)&code[0];

    prog_puts("PROGRAM: mmu_execstack starts, the code is on the stack at 0x");
    prog_put_hex((unsigned long)(unsigned long)&code[0], 16);
    prog_puts("\n");
    prog_puts("PROGRAM: mmu_execstack runs the stack page\n");

    body();

    /* Not reached: the fetch of the page is the fault of the program. */
    prog_puts("PROGRAM: the stack page executed, the policy is not there\n");

    prog_exit(1);

    return 0;
}
