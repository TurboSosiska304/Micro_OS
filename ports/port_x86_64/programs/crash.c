/*
 * Micro OS — crash, the program that proves a fault is not the end.
 *
 * A program of ring 3 has page tables of its own and the kernel is not
 * mapped in them. The write below goes to address zero, which is not a page
 * of the program: the processor raises a page fault in ring 3, the handler of
 * the port prints the line of the failure and takes the program down, and the
 * shell that ran it goes on to the next command. A machine that stopped at
 * the first bad pointer of the first bad program would not have protection;
 * it would have a note about it.
 *
 * The program is built and installed the same way as hello: a file in /bin
 * with the execute bit, read back through the VFS and entered by execve.
 */
#include "prog.h"

int crash_main(int argc, char *const argv[])
{
    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: crash writes to address zero\n");

    /* Volatile, so the compiler keeps the store the program is about. */
    *(volatile unsigned long *)0 = 1;

    prog_puts("PROGRAM: crash did not fault\n");

    return 1;
}
