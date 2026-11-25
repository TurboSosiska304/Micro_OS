/*
 * Micro OS — mmu_stray, a program that touches a page it does not own.
 *
 * The window of a program is 16 MiB and only the pages the program asked for
 * are in it (ADR 0007 §1): the image at the base, the heap above it, the
 * mappings above that, and the stack with its room at the top. The address
 * below is in the middle of the empty part - above any heap this program
 * grows and below the room of the stack - so no page of it is behind that
 * address and no guard of any call is there either.
 *
 * The store meets a page that is not present, the port has no mapping to
 * make and no stack to grow (the address is not in the room), and the program
 * ends with EFAULT and the address in the log (ADR 0052). The line after the
 * store is the control of the test.
 */
#include "prog.h"

#define MMU_STRAY_ADDRESS 0x14000000ul

int mmu_stray_main(int argc, char *const argv[])
{
    volatile unsigned long *page = (volatile unsigned long *)MMU_STRAY_ADDRESS;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: mmu_stray starts, the page is at 0x");
    prog_put_hex(MMU_STRAY_ADDRESS, 16);
    prog_puts("\n");
    prog_puts("PROGRAM: mmu_stray touches a page it does not own\n");

    *page = 1ul;

    /* Not reached: the page is not there and the fault is the program. */
    prog_puts("PROGRAM: the stray page took the write, the window is not checked\n");

    prog_exit(1);

    return 0;
}
