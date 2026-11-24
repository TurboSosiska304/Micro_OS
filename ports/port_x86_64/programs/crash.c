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
 * `crash stack` is the second shape of the same failure (ADR 0048): the
 * recursion runs past the bottom of the stack and into the page under it,
 * which the port leaves unmapped on purpose. The fault is a write into that
 * page and not a write into the data of the program, and the shell is alive
 * after it.
 *
 * The program is built and installed the same way as hello: a file in /bin
 * with the execute bit, read back through the VFS and entered by execve.
 */
#include "prog.h"

#define STACK_FRAME_BYTES 512u

static volatile unsigned long g_stack_sink;

static int word_is(const char *word, const char *name)
{
    while (*word != '\0' && *name != '\0' && *word == *name) {
        word++;
        name++;
    }

    return *word == '\0' && *name == '\0';
}

static __attribute__((noinline)) void overrun(unsigned long depth)
{
    volatile unsigned char pad[STACK_FRAME_BYTES];

    pad[0] = (unsigned char)(depth & 0xFFu);
    pad[STACK_FRAME_BYTES - 1u] = (unsigned char)((depth >> 8) & 0xFFu);

    if (depth < 100000u) {
        overrun(depth + 1u);
    }

    g_stack_sink = g_stack_sink + pad[0];
}

int crash_main(int argc, char *const argv[])
{
    if (argc > 1 && word_is(argv[1], "stack")) {
        prog_puts("PROGRAM: crash overruns the stack\n");

        overrun(0);

        prog_puts("PROGRAM: crash did not fault on the stack\n");
        return 1;
    }

    prog_puts("PROGRAM: crash writes to address zero\n");

    /* Volatile, so the compiler keeps the store the program is about. */
    *(volatile unsigned long *)0 = 1;

    prog_puts("PROGRAM: crash did not fault\n");

    return 1;
}
