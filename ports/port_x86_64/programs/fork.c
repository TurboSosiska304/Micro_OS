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

/* One logical line is one write: a log from another core may land between
 * writes, but it must not be able to split the value this proof checks. */
static void value_line(const char *prefix, unsigned long value)
{
    char line[80];
    char digits[20];
    size_t length = 0;
    size_t count = 0;

    while (prefix[length] != '\0') {
        line[length] = prefix[length];
        length++;
    }

    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u);

    while (count != 0u) {
        line[length++] = digits[--count];
    }

    line[length++] = '\n';
    (void)prog_write(1, line, (unsigned long)length);
}

int fork_main(int argc, char *const argv[])
{
    int status = -1;
    long pid;

    (void)argc;
    (void)argv;

    value_line("PROGRAM: fork starts, value ", g_value);

    pid = prog_fork();

    if (pid == 0) {
        /* The child: it owns a copy of the page as soon as it writes. */
        g_value = 99;

        value_line("PROGRAM: child fork returned 0, value ", g_value);

        prog_exit(7);
        return 1;
    }

    if (pid < 0) {
        value_line("PROGRAM: fork failed with ", (unsigned long)(-pid));
        prog_exit(1);
        return 1;
    }

    value_line("PROGRAM: parent fork returned ", (unsigned long)pid);

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
    value_line("PROGRAM: parent sees value ", g_value);
    value_line("PROGRAM: waited, status ", (unsigned long)status);

    prog_exit(0);
    return 0;
}
