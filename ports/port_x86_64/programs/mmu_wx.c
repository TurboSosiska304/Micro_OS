/*
 * Micro OS — mmu_wx, the W^X policy of the pages a program asks for.
 *
 * The tables of the port have carried the rule since 1.0 (ADR 0007, NX and
 * W^X): a page is code or it is data, and no page is both. Until 2.0 the rule
 * was the audit of the image and of the kernel's own map; a program that
 * asked the kernel for a writable and executable mapping was given a page
 * with the two bits, and the audit of the next `exec` had nothing to say
 * about a page made after it (ADR 0052).
 *
 * The two calls below are the policy at the edge: `mmap(PROT_WRITE |
 * PROT_EXEC)` is refused with EINVAL (22) before a page is made, and the same
 * bytes with PROT_WRITE alone are a mapping the program may use. The program
 * writes the page it got and ends with 0: a refusal that also refused the
 * data mapping would fail the second line.
 */
#include "prog.h"

#define MMU_WX_PAGE 4096ul

static void report(const char *what, long rc)
{
    prog_puts("PROGRAM: ");
    prog_puts(what);
    prog_puts(" ");
    prog_put_dec((unsigned long)(rc < 0 ? -rc : rc));
    prog_puts("\n");
}

int mmu_wx_main(int argc, char *const argv[])
{
    volatile unsigned char *page;
    long mapped;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: mmu_wx starts\n");

    mapped = prog_mmap(0, MMU_WX_PAGE, KSYS_PROT_READ | KSYS_PROT_WRITE | KSYS_PROT_EXEC,
                       KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0);
    report("PROT_WRITE|PROT_EXEC is refused with", mapped);

    if (mapped >= 0) {
        prog_puts("PROGRAM: the map of a writable and executable page is allowed\n");
        prog_exit(1);
        return 1;
    }

    /* The same bytes with one bit less: data, writable and never code. */
    mapped = prog_mmap(0, MMU_WX_PAGE, KSYS_PROT_READ | KSYS_PROT_WRITE,
                       KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0);
    if (mapped < 0) {
        report("mmap of a writable page answers", mapped);
        return 1;
    }

    prog_puts("PROGRAM: the data mapping is at 0x");
    prog_put_hex((unsigned long)mapped, 16);
    prog_puts("\n");

    page = (volatile unsigned char *)(unsigned long)mapped;
    page[0] = 0x5Au;
    page[MMU_WX_PAGE - 1u] = 0xA5u;

    if (page[0] != 0x5Au || page[MMU_WX_PAGE - 1u] != 0xA5u) {
        prog_puts("PROGRAM: the data mapping did not keep the bytes\n");
        return 1;
    }

    prog_puts("PROGRAM: a page is code or it is data, and the call says so\n");

    prog_exit(0);

    return 0;
}
