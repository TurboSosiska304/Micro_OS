/*
 * Micro OS — fork, copy on write and the wait for a child.
 *
 * This program is the proof of K4 on the one port with an MMU. The variable
 * below is in the data of the program; the fork makes both the parent and the
 * child point at the same physical page, read-only. The child writes 99 and
 * faults: the handler copies its page and the child goes on. The parent still
 * reads 42, which is the whole of copy on write in one line.
 *
 * The child ends through exit(7); the parent waits for it, gets 7 back and
 * ends with 0. The order of the transcript is fixed by the wait: the lines on
 * the child's side of the fork are printed before the parent is awake again.
 */
#include "prog.h"

/*
 * The page a fork shares with the child and splits on the first write. The
 * type is four bytes wide because the format puts the data right after the
 * text at a four-byte boundary (program.ld), and the object of the program
 * has to be linked at the address the loader will copy it to.
 */
static volatile unsigned int g_value = 42;

int fork_main(int argc, char *const argv[])
{
    int status = -1;
    long pid;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: fork starts, value ");
    prog_put_dec(g_value);
    prog_puts("\n");

    pid = prog_fork();

    if (pid == 0) {
        /* The child: it owns a copy of the page as soon as it writes. */
        g_value = 99;

        prog_puts("PROGRAM: child fork returned 0, value ");
        prog_put_dec(g_value);
        prog_puts("\n");

        prog_exit(7);
        return 1;
    }

    if (pid < 0) {
        prog_puts("PROGRAM: fork failed with ");
        prog_put_dec((unsigned long)(-pid));
        prog_puts("\n");
        prog_exit(1);
        return 1;
    }

    prog_puts("PROGRAM: parent fork returned ");
    prog_put_dec((unsigned long)pid);
    prog_puts("\n");

    if (prog_waitpid(pid, &status, 0) < 0) {
        prog_puts("PROGRAM: waitpid failed\n");
        prog_exit(1);
        return 1;
    }

    /*
     * The parent never wrote the page, so it is the page it had before the
     * fork: 42 and not the 99 of the child. A kernel that shared the memory
     * without the fault of the first write would print 99 here.
     */
    prog_puts("PROGRAM: parent sees value ");
    prog_put_dec(g_value);
    prog_puts("\n");

    prog_puts("PROGRAM: waited, status ");
    prog_put_dec((unsigned long)status);
    prog_puts("\n");

    prog_exit(0);
    return 0;
}
