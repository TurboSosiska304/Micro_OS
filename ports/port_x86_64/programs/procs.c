/*
 * Micro OS — the processes of a program: fork, waitpid, a double fork and an
 * execve that replaces the child itself.
 *
 * This is the proof of the POSIX pair on the one port with an MMU, the way
 * programs/fork.c is the proof of copy on write. Four things are shown here
 * and each of them fails in its own way when the kernel is wrong:
 *
 *   1. a child computes a number and ends with it; the parent waits and reads
 *      the status - a wait that reaped the wrong task would answer nothing;
 *   2. a child of a child does the same one level down; the middle process
 *      waits for it and hands its own status up - the double fork, and the
 *      link of every status with the process that made it;
 *   3. a child calls execvp and the image replaces that child, not a second
 *      task: the line after the call never runs. A kernel whose execve starts
 *      another task would let the child go on and exit with 9 here, and the
 *      parent would read 9;
 *   4. every status travels back as the code of the process, through the
 *      fork of the library and the waitpid of the library.
 *
 * The output is one line per fact and the order is fixed by the waits: a
 * parent prints a status only after the child that ends with it is gone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

/* The work of the child: the sum of the first `count` numbers. */
static int sum_to(int count)
{
    int total = 0;
    int number;

    for (number = 1; number <= count; number++) {
        total += number;
    }

    return total;
}

int main(int argc, char *const argv[])
{
    int status = -1;
    pid_t pid;

    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("PROGRAM: procs starts, pid %d\n", (int)getpid());

    /* 1. The child computes 55 and its status is the number. */
    pid = fork();
    if (pid == 0) {
        int total = sum_to(10);

        printf("PROGRAM: child computed %d\n", total);
        exit(total);
    }

    if (pid < 0) {
        printf("PROGRAM: fork failed\n");
        return 1;
    }

    printf("PROGRAM: the parent got the pid %d\n", (int)pid);

    if (waitpid(pid, &status, 0) != pid) {
        printf("PROGRAM: waitpid of the first child failed\n");
        return 1;
    }

    printf("PROGRAM: first child status %d\n", WEXITSTATUS(status));

    /* 2. The double fork: a child, a grandchild, two waits and two statuses. */
    pid = fork();
    if (pid == 0) {
        pid_t grandchild = fork();

        if (grandchild == 0) {
            exit(3);
        }

        if (grandchild < 0) {
            exit(8);
        }

        if (waitpid(grandchild, &status, 0) != grandchild) {
            exit(7);
        }

        printf("PROGRAM: middle waited, status %d\n", WEXITSTATUS(status));
        exit(4);
    }

    if (pid < 0) {
        printf("PROGRAM: the second fork failed\n");
        return 1;
    }

    if (waitpid(pid, &status, 0) != pid) {
        printf("PROGRAM: waitpid of the middle process failed\n");
        return 1;
    }

    printf("PROGRAM: double fork status %d\n", WEXITSTATUS(status));

    /*
     * 3. execve replaces the child. The echo prints the line, so a kernel
     * that ran the program in a task of its own and returned here would show
     * the two lines of the child and the status 9 of the mistake.
     */
    pid = fork();
    if (pid == 0) {
        execvp("/bin/echo", (char *const[]){"echo", "PROGRAM: the exec child", NULL});

        printf("PROGRAM: execve returned\n");
        exit(9);
    }

    if (pid < 0) {
        printf("PROGRAM: the third fork failed\n");
        return 1;
    }

    if (waitpid(pid, &status, 0) != pid) {
        printf("PROGRAM: waitpid of the exec child failed\n");
        return 1;
    }

    printf("PROGRAM: exec child status %d\n", WEXITSTATUS(status));
    printf("PROGRAM: procs ends\n");

    return 0;
}
