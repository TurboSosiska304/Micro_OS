/*
 * Micro OS — the stream of a command that a program starts: `popen` and
 * `pclose`.
 *
 * A program of this system has every call a command needs and no shell to put
 * them together: `pipe` (27) makes the two ends, `fork` (17) makes the child,
 * `dup2` (28) puts one end where the child reads or writes, `execve` (25)
 * replaces the child's image with the program, and `waitpid` (19) reaps it.
 * The POSIX shape of the two calls is built on those five, and the one part
 * of a shell they cannot live without - the words of the line - is
 * `src/words.c`.
 *
 * What is not there is the shell itself: this system has no `/bin/sh`, so a
 * command with a `|`, a `>` or a `;` in it is handed to `execve` as a word and
 * fails with `ENOENT` instead of being interpreted. The command is at most
 * eight words of 512 bytes, because that is what one `execve` of the ABI
 * carries, and a longer one is refused with `E2BIG` rather than run with a
 * part of itself (docs/debts.md).
 *
 * A read of a pipe of this system sleeps until the bytes arrive and until the
 * writer has closed it (ADR 0035), so a `popen("...", "r")` gives a stream
 * that a `fgets` loop reads to its end with no `poll` of its own.
 *
 * The descriptor table of a task has four entries (docs/ports-table.md), and a
 * command holds two of them for as long as its stream is open: a program can
 * read one command at a time, and a second `popen` while the first is open is
 * the `EMFILE` of the call.
 *
 * The ends of the pipe come from `pipe()` of the library, which never hands
 * out the numbers 1 and 2 of the console (src/syscall.c): an end on the
 * number of the console would be a `printf` of the parent that travels down
 * the pipe of its child.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "ulibc_internal.h"

/* The status of a command that was not found or could not be started: the
 * convention of a shell (and the number POSIX names for `system`). */
#define COMMAND_NOT_FOUND 127

/* The commands that are running with a stream of the caller: `pclose` needs
 * the pid of the child that belongs to the stream, and a stream of this
 * library carries a descriptor and no room of its own. The table is as long
 * as the descriptors a task can have (four), which is how many commands can
 * be open at once on this machine. */
#define COMMANDS_MAX 4

static struct {
    FILE *stream;
    pid_t pid;
} g_commands_s[COMMANDS_MAX];

/*
 * The child of a command: its end of the pipe is where the command reads or
 * writes, and the image of the program replaces the copy of this one. A
 * command that does not start ends with 127 and not with a return: the parent
 * reads the end of the pipe and `pclose` answers the status a shell gives a
 * name it could not run.
 *
 * The streams of the parent were given to the kernel before the fork, so the
 * copy of their blocks here is empty and nothing of the parent travels into
 * the pipe.
 */
static void __attribute__((noreturn)) command_child(char *const words[], int to_stdout, int keep,
                                                    int drop)
{
    int target = to_stdout ? ULIBC_STDOUT : ULIBC_STDIN;

    if (dup2(keep, target) < 0) {
        _exit(COMMAND_NOT_FOUND);
    }

    /* A pipe takes the lowest free numbers, and the number it took may be the
     * one it is being put on (a program with 0 or 1 closed). That end is the
     * program's stream now and closing it would close the stream. */
    if (keep != target) {
        (void)close(keep);
    }
    if (drop != keep && drop != target) {
        (void)close(drop);
    }

    (void)execvp(words[0], words);

    _exit(COMMAND_NOT_FOUND);
}

FILE *popen(const char *command, const char *mode)
{
    char block[ULIBC_COMMAND_BYTES];
    char *words[ULIBC_COMMAND_WORDS + 1];
    int ends[2];
    int reading;
    int slot;
    int count;
    pid_t pid;
    FILE *stream;

    if (mode == NULL || (mode[0] != 'r' && mode[0] != 'w')) {
        errno = EINVAL;
        return NULL;
    }

    count = __ulibc_command_words(command, block, words, ULIBC_COMMAND_WORDS);
    if (count <= 0) {
        /* An empty command is nothing to run: POSIX gives it to the shell,
         * and this machine has no shell to give it to. */
        if (count == 0) {
            errno = EINVAL;
        }
        return NULL;
    }

    for (slot = 0; slot < COMMANDS_MAX; slot++) {
        if (g_commands_s[slot].stream == NULL) {
            break;
        }
    }
    if (slot == COMMANDS_MAX) {
        errno = EMFILE;
        return NULL;
    }

    reading = (mode[0] == 'r') ? 1 : 0;

    /* The bytes the parent had buffered leave for the console before the
     * child is a copy of this program: a flush after the fork would give the
     * parent's output to both copies (the console and the pipe). */
    (void)fflush(NULL);

    if (pipe(ends) != 0) {
        return NULL; /* pipe() set errno */
    }

    pid = fork();
    if (pid < 0) {
        /* `fork` answered -1 with errno; the two ends are of this process. */
        (void)close(ends[0]);
        (void)close(ends[1]);
        return NULL;
    }

    if (pid == 0) {
        if (reading) {
            command_child(words, 1, ends[1], ends[0]);
        } else {
            command_child(words, 0, ends[0], ends[1]);
        }
    }

    {
        int kept = reading ? ends[0] : ends[1];

        /* The end the parent does not read or write closes here: a read of
         * the pipe ends when the last writer is gone, and the child is that
         * writer (or the reader of the bytes the parent is about to write). */
        (void)close(reading ? ends[1] : ends[0]);

        stream = fdopen(kept, mode);
        if (stream == NULL) {
            (void)close(kept);
            (void)waitpid(pid, NULL, 0);
            return NULL;
        }
    }

    g_commands_s[slot].stream = stream;
    g_commands_s[slot].pid = pid;

    return stream;
}

int pclose(FILE *stream)
{
    int slot;
    int status = 0;
    pid_t pid;

    for (slot = 0; slot < COMMANDS_MAX; slot++) {
        if (g_commands_s[slot].stream == stream) {
            break;
        }
    }

    if (stream == NULL || slot == COMMANDS_MAX) {
        /* A stream that no `popen` made has no child to wait for. */
        errno = ECHILD;
        return -1;
    }

    pid = g_commands_s[slot].pid;
    g_commands_s[slot].stream = NULL;
    g_commands_s[slot].pid = 0;

    /* The last read of the stream is the end of the pipe, and the close of
     * the descriptor is what tells the command that its reader is gone. */
    (void)fclose(stream);

    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }

    return status;
}
