/*
 * Micro OS — mmu_stack, the stack of a program that grows.
 *
 * The stack of a program starts as 64 KiB at the top of the window (ADR 0007)
 * and the page under it is the growth point of ADR 0052: a store below the
 * stack that is still inside the room of the task makes the pages between
 * there and the stack, and the frame that a deep call needs is not a fault.
 * The room is one megabyte, and the ceiling of the task
 * (`KSYS_RLIMIT_MEMORY`, `ulimit -m`) is applied to the whole stack.
 *
 * The program reserves 128 KiB of its stack in one step - two pages more than
 * the initial stack - writes one byte of every page and reads them back. The
 * first store is two pages below the stack and the port grows the stack by
 * the thirty-two pages of the array at once, which is the line of the log the
 * smoke test reads; the loop touches the pages the growth already made and
 * must find every byte.
 *
 * A machine whose stack is the 64 KiB block of 1.9 dies here with EFAULT, and
 * the smoke test reads the fault instead of the line at the end.
 */
#include "prog.h"

#define MMU_STACK_BYTES (128ul * 1024ul)
#define MMU_PAGE 4096ul

static void put_number(const char *text, unsigned long value)
{
    prog_puts(text);
    prog_put_dec(value);
    prog_puts("\n");
}

int mmu_stack_main(int argc, char *const argv[])
{
    volatile unsigned char block[MMU_STACK_BYTES];
    unsigned long index;
    unsigned long sum = 0;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: mmu_stack starts, the array is at 0x");
    prog_put_hex((unsigned long)(unsigned long)&block[0], 16);
    prog_puts("\n");

    /* One store of the first byte of every page: the first one is the fault
     * that grows the stack and the rest are pages it already made. */
    for (index = 0; index < MMU_STACK_BYTES; index += MMU_PAGE) {
        block[index] = (unsigned char)((index / MMU_PAGE) + 1u);
    }

    for (index = 0; index < MMU_STACK_BYTES; index += MMU_PAGE) {
        sum += block[index];
    }

    put_number("PROGRAM: the pages of the stack read back ", sum);
    put_number("PROGRAM: the stack grew by bytes ", MMU_STACK_BYTES);

    if (sum != (unsigned long)(1u + 32u) * 32ul / 2ul) {
        prog_puts("PROGRAM: the stack did not keep the bytes\n");
        return 1;
    }

    prog_puts("PROGRAM: a stack of 128 KiB above the first page works\n");

    prog_exit(0);

    return 0;
}
