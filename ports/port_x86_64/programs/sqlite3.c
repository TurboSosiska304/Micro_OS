/*
 * Micro OS — the shell of SQLite with the console and the input of this
 * machine.
 *
 * `shell.c` of the amalgamation is the program of `/bin/sqlite3`; this file
 * is the little that is of the machine and not of SQLite:
 *
 *  - **the console**. The descriptors 1 and 2 are the console of the kernel
 *    only while they are not open, and SQLite opens the database and the
 *    journal on the lowest free descriptor. A program that opened a database
 *    on 1 would write its own output into the database. The two descriptors
 *    are therefore opened on `/dev/tty0` before SQLite sees a file, and the
 *    database takes 0 and the next file 3 (the table of a task has four);
 *  - **the input**. The machine has four descriptors, and a script of stdin
 *    plus a database plus two files of a journal do not fit. The script is
 *    read whole into the arena, the descriptor 0 is closed, and `stdin` is a
 *    stream over the memory (`fmemopen`); a `.read` of the shell then has a
 *    descriptor of its own (0) for as long as it runs;
 *  - **the journal**. With one descriptor beside the database and the
 *    console, the journal of a transaction cannot be a second file; the
 *    program asks SQLite for a journal in memory (`journal_mode=MEMORY`)
 *    before the first command. The VFS of the port implements the file
 *    journal as well (DELETE, PERSIST, TRUNCATE) and the host test exercises
 *    it; the reason of the difference is the four descriptors of a task, and
 *    it is a row in docs/debts.md.
 *
 * The four descriptors of the program, then: 0 the database (or a `.read`
 * when one is open), 1 and 2 the console, 3 free for the next file SQLite
 * opens.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>

#include "ulibc.h"

/* shell.c is compiled whole by third_party/sqlite/port/sqlite_shell.c, which
 * renames its entry to this name; the console and the input are ready by the
 * time it runs. */
int sqlite3_shell_main(int argc, char **argv);

/* The largest script of stdin. The arena of the program is 768 KiB and
 * SQLite's page cache takes a part of it; a script of a quarter of a
 * megabyte is far more than a fixture of the tests and leaves the library
 * the rest. A bigger script is refused with a sentence and not truncated. */
#define SCRIPT_MAX (256u * 1024u)
#define SCRIPT_STEP 4096u

/*
 * Read the whole of descriptor 0. The pipe of the shell answers EAGAIN while
 * its producer still holds the other end (ADR 0014), so the loop waits the
 * condition out instead of reading a short script as the end of the file.
 */
static char *read_script(size_t *length)
{
    size_t capacity = SCRIPT_STEP;
    size_t used = 0;
    char *buffer = (char *)malloc(capacity);

    if (buffer == NULL) {
        return NULL;
    }

    for (;;) {
        long count;

        if (used == capacity) {
            char *grown;

            if (capacity >= SCRIPT_MAX) {
                free(buffer);
                return NULL;
            }
            capacity = (capacity * 2u > SCRIPT_MAX) ? SCRIPT_MAX : capacity * 2u;
            grown = (char *)realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                return NULL;
            }
            buffer = grown;
        }

        count = read(0, buffer + used, capacity - used);
        if (count > 0) {
            used += (size_t)count;
            continue;
        }
        if (count == 0) {
            break;
        }
        if (count == -EAGAIN) { /* the producer of the pipe is still there */
            sleep(0);
            continue;
        }
        break; /* EBADF: the program was started with no input at all */
    }

    *length = used;
    return buffer;
}

/*
 * The console of the program is the device `/dev/tty0`, and it is put on the
 * descriptors 1 and 2 by hand: the table of a program is inherited from the
 * stage of the shell that started it, and the numbers 1 and 2 are not free
 * just because the stage did not ask for a redirect. `dup2` makes the two
 * slots the console whatever they held, and the descriptor of the open is
 * given back. A machine without the device (the host of the test, a port
 * with no devfs) keeps the kernel's virtual console on the two numbers.
 */
static void reserve_console(void)
{
    int device = open("/dev/tty0", ULIBC_O_WRONLY);

    if (device >= 0) {
        if (device != ULIBC_STDOUT) {
            (void)dup2(device, ULIBC_STDOUT);
        }
        if (device != ULIBC_STDERR) {
            (void)dup2(device, ULIBC_STDERR);
        }
        if (device != ULIBC_STDOUT && device != ULIBC_STDERR) {
            (void)close(device);
        }
    }

    /*
     * The program keeps the three descriptors of its interface and nothing
     * above them: the slot 0 is the database (or a `.read` of the shell) and
     * the slot 3 is the next file SQLite opens. A descriptor of the stage
     * that outlived the redirect would take one of the two.
     */
    (void)close(3);
}

/* The arguments of the shell with the one command of the port in front. */
static char **with_journal_command(int argc, char **argv)
{
    static char journal[] = "PRAGMA journal_mode=MEMORY";
    char **out = (char **)malloc(((size_t)argc + 3u) * sizeof(char *));
    int index;

    if (out == NULL) {
        return NULL;
    }

    out[0] = argv[0];
    out[1] = (char *)"-cmd";
    out[2] = journal;
    for (index = 1; index < argc; index++) {
        out[index + 2] = argv[index];
    }
    out[argc + 2] = NULL;

    return out;
}

int main(int argc, char **argv)
{
    size_t length = 0;
    char *script;
    char **args;

    script = read_script(&length);

    /* The descriptor of the input is not needed any more: the script is in
     * the memory and the slot is the one a `.read` of the shell will use. */
    (void)close(0);

    reserve_console();

    if (script == NULL) {
        printf("sqlite3: the input does not fit the arena of the program (%u bytes)\n",
               (unsigned)SCRIPT_MAX);
        return 1;
    }

    stdin = fmemopen(script, length, "r");
    if (stdin == NULL) {
        printf("sqlite3: no memory for the input stream\n");
        return 1;
    }

    args = with_journal_command(argc, argv);
    if (args == NULL) {
        printf("sqlite3: no memory for the arguments\n");
        return 1;
    }

    return sqlite3_shell_main(argc + 2, args);
}
