/*
 * Micro OS — spawn, the program that starts another one with an environment
 * of its own.
 *
 * It is the proof of the third argument of `execve` from the user side: the
 * vectors below are memory of this program, the kernel copies them at the
 * edge, and `/bin/hello` - a second task, entered with the environment the
 * kernel copied - prints the two words back. The program is not a shell
 * builtin and not a call of the kernel: it is a file of `/bin` that asks for
 * another file of `/bin`, which is what a userland of this size is for.
 *
 * The call waits for the program it started (the machine has no process to
 * exit into), so the status of this task is the status of the program only
 * when it finished well.
 */
#include "ulibc.h"

int main(int argc, char *const argv[], char *const envp[])
{
    char *hello_argv[] = {(char *)"hello", (char *)"from-spawn", NULL};
    char *hello_envp[] = {(char *)"PATH=/custom", (char *)"MODE=demo", NULL};
    int rc;

    (void)argc;
    (void)argv;
    (void)envp;

    printf("spawn: starting /bin/hello with its own environment\n");

    rc = execve("/bin/hello", hello_argv, hello_envp);
    if (rc < 0) {
        printf("spawn: execve failed with %d\n", -rc);
        return ULIBC_EXIT_FAILURE;
    }

    return ULIBC_EXIT_SUCCESS;
}
