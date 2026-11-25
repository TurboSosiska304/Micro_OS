/*
 * Micro OS — forksmoke, the fork of a program of 64 MiB (ADR 0057).
 *
 * `fork()` copies the page tables of the parent and shares its frames: the
 * pages are not copied until one of the two sides writes them, and the child
 * reads the memory of the parent as it was at the call. The promise of 2.0 is
 * that this is cheap - a program of 64 MiB forks in a millisecond - and this
 * program is the one that proves it on the machine:
 *
 *   - it maps 64 MiB of anonymous memory and writes one byte of every page,
 *     so the pages are real and not a promise of the mapping;
 *   - it forks: the port shares the frames, marks both copies read-only and
 *     writes the line `x86-64: fork (pid N) shares K page(s) in T us` in the
 *     log, which is the measurement of the call;
 *   - the child writes four of the pages, and each of those writes is one
 *     `copy on write` line and one frame of its own;
 *   - the parent reads the same four addresses and must find the value it
 *     wrote before the fork: the copy of the child is a copy and not a view;
 *   - the parent waits for the child and the status is 0.
 *
 * The program is of the port and not of the library: the 64 MiB are one
 * anonymous mapping (`prog_mmap`), the pages are touched with a pattern and
 * the fork is the call of the ABI. The number of the call is the kernel's
 * line, because a clock of a program of this system counts milliseconds and
 * the fork has to be read in microseconds.
 */
#include "prog.h"

#define FORK_SMOKE_BYTES (64ul * 1024ul * 1024ul)
#define FORK_SMOKE_PAGE 4096ul
#define FORK_SMOKE_COPIES 4u

/* The pages the child writes and the parent reads back: four is enough to be
 * a count and few enough to read in the log, one line each. */
static const unsigned long g_copies[FORK_SMOKE_COPIES] = {
    0ul,
    FORK_SMOKE_BYTES / 4ul,
    FORK_SMOKE_BYTES / 2ul,
    FORK_SMOKE_BYTES - FORK_SMOKE_PAGE,
};

static void say(const char *text)
{
    prog_puts("PROGRAM: ");
    prog_puts(text);
    prog_puts("\n");
}

int forksmoke_main(int argc, char *const argv[])
{
    volatile unsigned char *memory;
    unsigned long pages = FORK_SMOKE_BYTES / FORK_SMOKE_PAGE;
    unsigned long at;
    long mapped;
    long pid;
    int status = -1;
    unsigned int i;
    unsigned int old_seen = 0;
    unsigned int new_seen = 0;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: forksmoke starts, ");
    prog_put_dec(FORK_SMOKE_BYTES);
    prog_puts(" bytes of memory in ");
    prog_put_dec(pages);
    prog_puts(" pages\n");

    /*
     * One fork before the memory is mapped: the first fork of a boot pays the
     * one-time work of the kernel - the slab of the records of a space, the
     * guard of the first task stack in a chunk, the first block of the heap -
     * and that is not the call this program measures (the first fork of the
     * 64 MiB was 879 us and the second 501 on the machine of the gate). A
     * warm-up of twenty pages is the honest way to measure the call and not
     * the kernel's first steps.
     */
    pid = prog_fork();
    if (pid == 0) {
        prog_exit(0);
        return 0;
    }

    if (pid < 0 || prog_waitpid(pid, &status, 0) < 0) {
        say("the warm-up fork failed");
        return 1;
    }

    if (status != 0) {
        say("the warm-up child did not end with 0");
        return 1;
    }

    say("the warm-up fork is done, the 64 MiB come now");

    mapped = prog_mmap(NULL, FORK_SMOKE_BYTES, KSYS_PROT_READ | KSYS_PROT_WRITE,
                       KSYS_MAP_PRIVATE | KSYS_MAP_ANONYMOUS, -1, 0);
    if (mapped < 0) {
        prog_puts("PROGRAM: the memory is refused with ");
        prog_put_dec((unsigned long)(-mapped));
        prog_puts("\n");
        return 1;
    }

    memory = (volatile unsigned char *)(unsigned long)mapped;

    /* Every page is touched: a page of the mapping that nobody wrote is not a
     * frame of the machine and there would be nothing to share. */
    for (at = 0; at < FORK_SMOKE_BYTES; at += FORK_SMOKE_PAGE) {
        memory[at] = (unsigned char)((at / FORK_SMOKE_PAGE) & 0xFFu);
    }

    for (i = 0; i < FORK_SMOKE_COPIES; i++) {
        memory[g_copies[i]] = (unsigned char)(0x40u + i);
    }

    say("the 64 MiB are touched and the fork comes");

    pid = prog_fork();
    if (pid == 0) {
        /* The child writes the four pages: each write is a copy of the page
         * it wrote and not a change of the memory of the parent. */
        for (i = 0; i < FORK_SMOKE_COPIES; i++) {
            memory[g_copies[i]] = (unsigned char)(0xA0u + i);
        }

        for (i = 0; i < FORK_SMOKE_COPIES; i++) {
            if (memory[g_copies[i]] != (unsigned char)(0xA0u + i)) {
                say("the child did not read its own writes");
                prog_exit(1);
            }
        }

        say("the child wrote four pages and reads them");
        prog_exit(0);

        return 0;
    }

    if (pid < 0) {
        prog_puts("PROGRAM: the fork failed with ");
        prog_put_dec((unsigned long)(-pid));
        prog_puts("\n");
        return 1;
    }

    say("the parent is back from the fork");

    /* The parent reads the four pages: it must see what it wrote, and the
     * child must see its own writes - the copy on write is two pages and not
     * one. */
    for (i = 0; i < FORK_SMOKE_COPIES; i++) {
        unsigned char seen = memory[g_copies[i]];

        if (seen == (unsigned char)(0x40u + i)) {
            old_seen++;
        } else if (seen == (unsigned char)(0xA0u + i)) {
            new_seen++;
        }
    }

    if (old_seen == FORK_SMOKE_COPIES && new_seen == 0) {
        say("the parent sees the old pages, the child its own writes");
    } else {
        prog_puts("PROGRAM: the parent sees ");
        prog_put_dec(old_seen);
        prog_puts(" old and ");
        prog_put_dec(new_seen);
        prog_puts(" new page(s)\n");
        return 1;
    }

    if (prog_waitpid(pid, &status, 0) < 0) {
        say("the wait for the child failed");
        return 1;
    }

    prog_puts("PROGRAM: the child ended with ");
    prog_put_dec((unsigned long)(status & 0xFF));
    prog_puts("\n");

    if (status != 0) {
        return 1;
    }

    say("the fork of 64 MiB is a copy of the pages and the two sides are two");

    prog_exit(0);

    return 0;
}
