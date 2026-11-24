/*
 * Micro OS — mmu_kernel, a program that writes the kernel.
 *
 * The kernel lives at the top of the address space (ADR 0007, "The higher
 * half: taken") and every page of it is supervisor-only: the entry carries no
 * user bit at any level of the walk, so ring 3 may not read a byte of it and
 * a store is a page fault at the first level the processor refuses. The text
 * of the kernel is also read-only in ring 0, and the store below is aimed at
 * the first page of the image.
 *
 * The error code is 0x7 - present, write, ring 3 - which is the proof that
 * the page exists and that its supervisor bit is what stopped the store: a
 * page that was not mapped at all would answer 0x6 (write, ring 3, no
 * present) and the kernel would be missing rather than protected.
 *
 * SMEP and SMAP, when the processor has them, are the other direction of the
 * same boundary (ADR 0052) and are not what this program tests: it is ring 3
 * reaching for ring 0, and the supervisor bit is the answer on every machine.
 */
#include "prog.h"

#define MMU_KERNEL_BASE 0xFFFFFFFF80000000ul

int mmu_kernel_main(int argc, char *const argv[])
{
    volatile unsigned long *page = (volatile unsigned long *)MMU_KERNEL_BASE;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: mmu_kernel starts, the kernel is at 0x");
    prog_put_hex(MMU_KERNEL_BASE, 16);
    prog_puts("\n");
    prog_puts("PROGRAM: mmu_kernel writes the first page of the kernel\n");

    *page = 1ul;

    /* Not reached: the supervisor bit of the page is the fault. */
    prog_puts("PROGRAM: the kernel text took the write, the machine is not protected\n");

    prog_exit(1);

    return 0;
}
