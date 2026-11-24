/*
 * Micro OS — mmu_heap, a program that asks for more memory than its limit.
 *
 * The memory of a task has a ceiling (KSYS_RLIMIT_MEMORY, `ulimit -m`): the
 * heap is charged to it as `brk` grows it, and an anonymous mapping is
 * counted when it is made (ADR 0052, kernel/mm.h). The program lowers its own
 * limit to 128 KiB and asks for twice that in the two ways a program of this
 * system has:
 *
 *   - `brk` past the limit answers ENOMEM (12) and the break stays where it
 *     was, which is the refusal of POSIX;
 *   - `mmap` of an anonymous private mapping past the limit answers ENOMEM
 *     for the same reason.
 *
 * Neither is a fault: the program prints both answers and ends with 0, and
 * `make mmu-smoke` reads the "still alive" line. A machine whose limit was a
 * comment would move the break, take the mapping, and the program would end
 * with the failure of the control line.
 */
#include "prog.h"

#define MMU_LIMIT_BYTES (128ul * 1024ul)
#define MMU_ASK_BYTES (256ul * 1024ul)

static void report(const char *what, long rc)
{
    prog_puts("PROGRAM: ");
    prog_puts(what);
    prog_puts(" ");
    prog_put_dec((unsigned long)(rc < 0 ? -rc : rc));
    prog_puts("\n");
}

int mmu_heap_main(int argc, char *const argv[])
{
    unsigned long before;
    unsigned long limit = 0;
    long rc;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: mmu_heap starts\n");

    /* The limit only goes down, and this program is a task of its own: the
     * 128 KiB below is the ceiling of this image and of no other. */
    rc = prog_setlimit(KSYS_RLIMIT_MEMORY, MMU_LIMIT_BYTES);
    if (rc != 0) {
        report("mmu_heap cannot lower the limit:", rc);
        return 1;
    }

    if (prog_getlimit(KSYS_RLIMIT_MEMORY, &limit) != 0) {
        prog_puts("PROGRAM: mmu_heap cannot read the limit\n");
        return 1;
    }

    prog_puts("PROGRAM: the limit is ");
    prog_put_dec(limit);
    prog_puts(" bytes\n");

    /* The break as it is, without moving it: `brk(0)` is the question. */
    before = (unsigned long)prog_brk(0);
    prog_puts("PROGRAM: the heap ends at 0x");
    prog_put_hex(before, 16);
    prog_puts("\n");

    report("brk past the limit answers", prog_brk(before + MMU_ASK_BYTES));

    /* The same ceiling for a mapping with no file behind it. */
    report("mmap past the limit answers",
           prog_mmap(0, MMU_ASK_BYTES, KSYS_PROT_READ | KSYS_PROT_WRITE,
                     KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0));

    /* The break did not move: the refusal was the call and not a state. */
    if ((unsigned long)prog_brk(0) != before) {
        prog_puts("PROGRAM: the heap moved, the refusal was not final\n");
        return 1;
    }

    prog_puts("PROGRAM: the heap stops at the limit with ENOMEM and the program is alive\n");

    prog_exit(0);

    return 0;
}
