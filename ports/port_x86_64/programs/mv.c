/*
 * Micro OS — mv, a name moved through the call of the kernel.
 *
 * The program is the atomic rename of the VFS and nothing else: one source,
 * one destination, no copy and no tree. A volume that cannot rename answers
 * with the error of the call, and the sentence names it; a directory moves
 * only inside its own parent, which is the limit the filesystems share.
 */
#include <errno.h>
#include <stdio.h>

#include "ulibc.h"

int main(int argc, char *const argv[], char *const envp[])
{
    (void)envp;

    if (argc != 3) {
        static const char usage[] = "usage: mv <from> <to>\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    if (rename(argv[1], argv[2]) != 0) {
        printf("mv: %s: error %d\n", argv[1], errno);
        return ULIBC_EXIT_FAILURE;
    }

    return ULIBC_EXIT_SUCCESS;
}
