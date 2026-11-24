/*
 * Micro OS — the gate of `popen`, `pclose` and `system` on the machine.
 *
 * The three calls are the library's (`components/libc/user/src/command.c`):
 * a pipe, a fork, an `execve` of the child and a `waitpid` of the parent.
 * The programs this file starts are the tools of the same directory - and
 * `/bin/echo` of the image - so the answers of the run are the answers of
 * programs that were built by the SDK as well, and not of a stub.
 *
 * It is the one program of this directory that is ours: the eleven beside it
 * are the release of sbase, byte for byte (docs/ports-table.md).
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TOOL(name) "/bin/ports/" name

int main(int argc, char *const argv[], char *const envp[])
{
    char line[128];
    FILE *stream;
    int status;

    (void)argc;
    (void)argv;
    (void)envp;

    /* --- system: fork, the image of the child, the wait --- */
    status = system(TOOL("true"));
    printf("system true: %d\n", status);

    status = system(TOOL("false"));
    printf("system false: %d\n", status);

    /* The words of a line are split the way the shell splits them: the quote
     * keeps the blank of the second word in it. */
    status = system(TOOL("echo") " first 'second word'");
    printf("system echo: %d\n", status);

    /* A program that is not there is the 127 of a shell and not a crash. */
    status = system(TOOL("no-such-command"));
    printf("system missing: %d\n", status);

    /* An empty line ran nothing and ended with 0; a NULL asks whether a
     * command can be run at all, which is the 1 of POSIX. */
    printf("system empty: %d\n", system(""));
    printf("system null: %d\n", system(NULL));

    /* --- popen, the read side: the program writes, this one reads --- */
    stream = popen(TOOL("echo") " one two", "r");
    if (stream == NULL) {
        printf("popen read: failed, errno %d\n", errno);
        return 1;
    }
    if (fgets(line, sizeof(line), stream) != NULL) {
        size_t length = strlen(line);

        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }
        printf("popen read: [%s]\n", line);
    }
    printf("pclose read: %d\n", pclose(stream));

    /* The status of a command that did not start travels to `pclose`. */
    stream = popen(TOOL("no-such-command"), "r");
    if (stream == NULL) {
        printf("popen missing: failed, errno %d\n", errno);
        return 1;
    }
    printf("popen missing: eof %d\n", fgetc(stream) == EOF);
    printf("pclose missing: %d\n", pclose(stream));

    /* --- popen, the write side: `tee` copies the stream to a file --- */
    stream = popen(TOOL("tee") " /command.txt", "w");
    if (stream == NULL) {
        printf("popen write: failed, errno %d\n", errno);
        return 1;
    }
    fputs("written through a pipe\n", stream);
    printf("pclose write: %d\n", pclose(stream));

    /* The file is the proof that the child read what the parent wrote. */
    stream = fopen("/command.txt", "r");
    if (stream == NULL) {
        printf("read back: open failed, errno %d\n", errno);
        return 1;
    }
    while (fgets(line, sizeof(line), stream) != NULL) {
        printf("read back: %s", line);
    }
    fclose(stream);

    return 0;
}
