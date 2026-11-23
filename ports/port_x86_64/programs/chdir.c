/*
 * Micro OS — chdir, a child and the directory of the task.
 *
 * The call 48 moves the working directory of the task, and a program that
 * starts another program carries it: the child of the fork below is a copy of
 * the record of the task, so it begins in /etc, and `pwd` - a program of
 * another image - prints /etc from its own call of `getcwd`. Before the call
 * existed the library kept the directory itself and the child began in the
 * directory of the shell, which is the defect this program is here to catch.
 *
 * The last line is the directory the program began in and not the one it
 * moved to: a program has a record of its own, and the shell that started it
 * keeps its own.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *here(char *buffer, size_t size)
{
    if (getcwd(buffer, size) == NULL) {
        return "(no directory)";
    }

    return buffer;
}

int main(int argc, char *const argv[], char *const envp[])
{
    char path[VFS_PATH_MAX];
    char start[VFS_PATH_MAX];
    int status = -1;
    long pid;

    (void)argc;
    (void)argv;
    (void)envp;

    if (strlen(here(start, sizeof(start))) >= sizeof(start)) {
        return ULIBC_EXIT_FAILURE;
    }

    printf("chdir: the program begins in %s\n", start);

    if (chdir("/etc") != 0) {
        printf("chdir: /etc: %d\n", errno);
        return ULIBC_EXIT_FAILURE;
    }
    printf("chdir: cd /etc says %s\n", here(path, sizeof(path)));

    /* A name that is not a directory is refused and the directory stays. */
    if (chdir("/etc/setting") != 0) {
        printf("chdir: a file is not a directory: %d\n", errno);
    } else {
        printf("chdir: a file was taken as a directory\n");
    }
    printf("chdir: after the refusal %s\n", here(path, sizeof(path)));

    pid = fork();
    if (pid == 0) {
        /* The child: a new image over the record of this task. `pwd` asks
         * the kernel for the directory it started in. */
        char *const child_argv[] = {"pwd", NULL};

        execve("/bin/pwd", child_argv, NULL);
        printf("chdir: exec: %d\n", errno);
        _exit(1);
    }

    if (pid < 0) {
        printf("chdir: fork: %d\n", errno);
        return ULIBC_EXIT_FAILURE;
    }

    if (waitpid((int)pid, &status, 0) < 0) {
        printf("chdir: wait: %d\n", errno);
        return ULIBC_EXIT_FAILURE;
    }

    printf("chdir: the child ended with %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    printf("chdir: the parent is still in %s\n", here(path, sizeof(path)));

    if (chdir("/") != 0) {
        printf("chdir: /: %d\n", errno);
        return ULIBC_EXIT_FAILURE;
    }

    printf("chdir: done\n");

    return ULIBC_EXIT_SUCCESS;
}
