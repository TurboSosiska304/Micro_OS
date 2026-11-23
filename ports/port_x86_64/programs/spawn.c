/*
 * Micro OS — spawn, the program that starts another one with an environment
 * of its own.
 *
 * It is the proof of the third argument of `execve` from the user side: the
 * vectors below are memory of this program, the kernel copies them at the
 * edge, and `/bin/hello` - the same task, with the image and the environment
 * the kernel copied - prints the two words back. The program is not a shell
 * builtin and not a call of the kernel: it is a file of `/bin` that asks for
 * another file of `/bin`, which is what a userland of this size is for.
 *
 * The call does not come back: the image of this file is replaced by the
 * image of hello (POSIX), so `hello` is the program that runs from the call
 * on and this file's `main` never returns. The status of that task is the
 * status of hello, and the shell that started spawn waits for it.
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
