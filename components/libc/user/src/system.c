/*
 * Micro OS — a command of the machine as a process of its own: `system`.
 *
 * The call is `popen` without the stream: `fork` (17) makes the child, the
 * image of the program named by the first word of the line replaces the child
 * (`execve`, 25), and `waitpid` (19) answers its status. The words of the line
 * are `src/words.c`, and the limit is the one of one `execve` of the ABI:
 * eight words of 512 bytes (docs/debts.md).
 *
 * This system has no `/bin/sh`, so the line is not a script: `system("echo hi
 * > /tmp/f")` runs `/bin/echo` with the words `hi`, `>` and `/tmp/f` and not a
 * shell that opens the file. What the call is for - a program running a
 * command and reading its status - is here; a line of a shell is a debt.
 *
 * A caller whose `SIGINT` or `SIGQUIT` is not the default does not have them
 * set aside while the command runs, as POSIX asks: the console of this machine
 * is one, and the shell above the program already owns the interrupt (the
 * same sentence is in docs/debts.md).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ulibc_internal.h"

/* The status of a command that was not found or could not be started: the
 * convention of a shell, and the number POSIX names for `system`. */
#define COMMAND_NOT_FOUND 127

int system(const char *command)
{
    char block[ULIBC_COMMAND_BYTES];
    char *words[ULIBC_COMMAND_WORDS + 1];
    int count;
    int status = 0;
    pid_t pid;

    if (command == NULL) {
        /* POSIX asks whether a shell is there at all; this machine's answer
         * is "there is a way to run a command", and the way is the call. */
        return 1;
    }

    count = __ulibc_command_words(command, block, words, ULIBC_COMMAND_WORDS);
    if (count < 0) {
        return -1;
    }
    if (count == 0) {
        return 0; /* an empty command ended with 0, as a shell does */
    }

    (void)fflush(NULL);

    pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        (void)execvp(words[0], words);
        _exit(COMMAND_NOT_FOUND);
    }

    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }

    return status;
}
