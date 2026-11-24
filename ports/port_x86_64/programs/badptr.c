/*
 * Micro OS — badptr, the program that hands the kernel pointers it may not
 * read.
 *
 * A program of ring 3 is not trusted with the kernel, so a pointer it passes
 * to a call is checked at the edge of the system. The addresses below are the
 * three ways a pointer is not the memory of the program:
 *
 *   - 0xdead0000 is outside the window of the programs altogether;
 *   - the last four bytes of the window with a request of eight run past its
 *     end, where the next byte belongs to nobody;
 *   - the page below the stack is inside the room of ADR 0052 and is not
 *     mapped yet: a *store of the program* there would grow the stack, and a
 *     pointer a call hands to the kernel is not that store - the call cannot
 *     make a page for an address the program has not used, so it is EFAULT
 *     like the two above.
 *
 * The answer of every call is EFAULT (14 with the minus sign), and the machine
 * is alive after all of them: the last line is written through the same call
 * with a pointer that is valid. A kernel that read the first address in ring 0
 * would have stopped at the first line, which is what makes this program a
 * test of the edge and not of a string.
 */
#include "prog.h"

#define FAR_POINTER ((void *)0xdead0000ul)
#define EDGE_POINTER ((void *)(0x03000000ul - 4ul))
#define GUARD_POINTER ((void *)0x02fef000ul)

/* One line: the name of the call and the number it answered. */
static void report(const char *what, long rc)
{
    prog_puts("PROGRAM: ");
    prog_puts(what);
    prog_puts(" ");
    prog_put_dec((unsigned long)(rc < 0 ? -rc : rc));
    prog_puts("\n");
}

int badptr_main(int argc, char *const argv[])
{
    long rc;
    int fd;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: badptr starts\n");

    report("write far", prog_write(1, FAR_POINTER, 8));
    report("write edge", prog_write(1, EDGE_POINTER, 8));
    report("write guard", prog_write(1, GUARD_POINTER, 8));
    report("stat far", prog_stat("/bin/hello", FAR_POINTER));

    fd = (int)prog_open("/bin/hello", KSYS_O_RDONLY);
    if (fd < 0) {
        report("open", fd);
        return 1;
    }

    report("read far", prog_read(fd, FAR_POINTER, 8));
    (void)prog_close(fd);

    /*
     * The same write with a string the program owns: the edge refused the
     * bad addresses, not the call. A machine that answered EFAULT to every
     * write would pass the lines above and fail this one.
     */
    rc = prog_write(1, "ok", 2);
    if (rc != 2) {
        report("good write", rc);
        return 1;
    }

    prog_puts("PROGRAM: the good pointer still writes\n");

    prog_exit(0);

    return 0;
}
