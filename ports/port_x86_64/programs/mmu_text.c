/*
 * Micro OS — mmu_text, a program that writes its own text.
 *
 * The text of a program is a page of code: readable and executable, never
 * writable (ADR 0007, NX and W^X, and ADR 0052 for the policy of the page).
 * The program takes the address of one of its own functions and stores a byte
 * into it. The store meets a page without the write bit, the processor raises
 * a page fault from ring 3 with the error code of a write (0x7: present,
 * write, user), and the port ends the task with EFAULT and the address in the
 * log. The shell that started the program answers the next command.
 *
 * The line after the store is the control of the test: a machine that let the
 * write through prints it, and `make mmu-smoke` looks for it and must not find
 * it.
 */
#include "prog.h"

/*
 * A function of the text, kept in the image although nothing calls it: the
 * address below is what the store is aimed at, and a compiler that folded an
 * unused function away would aim the store at something else.
 */
__attribute__((noinline, used)) static void mmu_text_code(void)
{
    prog_puts("PROGRAM: the text of the program ran\n");
}

int mmu_text_main(int argc, char *const argv[])
{
    unsigned long text = (unsigned long)(unsigned long)&mmu_text_code;
    volatile unsigned char *page = (volatile unsigned char *)text;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: mmu_text starts, the text is at 0x");
    prog_put_hex(text, 16);
    prog_puts("\n");
    prog_puts("PROGRAM: mmu_text writes its own text\n");

    *page = 0x90u;

    /* Not reached: the store is the fault of the program. */
    prog_puts("PROGRAM: the text took the write, the policy is not there\n");

    prog_exit(1);

    return 0;
}
