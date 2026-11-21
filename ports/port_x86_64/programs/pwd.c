/*
 * Micro OS — pwd, the current directory of the task.
 *
 * The path comes from the call of the kernel and not from a variable of the
 * shell: the task keeps its own directory, and a program that starts in the
 * same place as the shell it was run from is the proof that the value
 * crossed the edge with the call (and not with the arguments).
 *
 * The shell of the machine has no relative path yet, so the answer of the
 * boot is `/` and a `cd` of the shell moves it for the programs after it.
 */
#include "ulibc.h"

int main(int argc, char *const argv[], char *const envp[])
{
    char path[VFS_PATH_MAX];
    int length;

    (void)argc;
    (void)argv;
    (void)envp;

    length = getcwd(path, sizeof(path));
    if (length < 0) {
        printf("pwd: error %d\n", -length);
        return ULIBC_EXIT_FAILURE;
    }

    printf("%s\n", path);

    return ULIBC_EXIT_SUCCESS;
}
