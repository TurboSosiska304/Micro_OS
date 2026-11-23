/*
 * Micro OS — rmdir, an empty directory removed through the call of the
 * kernel.
 *
 * The program makes one call for every word of its line and reports each
 * name that could not be removed. It does not walk a tree and it does not
 * read the entries: a directory that still holds a name is the error of the
 * filesystem (ENOTEMPTY), and `rm -r` is the command that takes the tree
 * down.
 */
#include <errno.h>
#include <stdio.h>

#include "ulibc.h"

#include <unistd.h>

int main(int argc, char *const argv[], char *const envp[])
{
    int status = 0;
    int i;

    (void)envp;

    if (argc < 2) {
        static const char usage[] = "usage: rmdir <directory>...\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    for (i = 1; i < argc; i++) {
        if (rmdir(argv[i]) != 0) {
            printf("rmdir: %s: error %d\n", argv[i], errno);
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
