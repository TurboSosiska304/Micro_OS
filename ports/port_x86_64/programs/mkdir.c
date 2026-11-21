/*
 * Micro OS — mkdir, a directory through the call of the kernel.
 *
 * The program makes one directory for every word of its line and reports
 * each one that could not be made. There is no `-p`: the parent has to exist,
 * and the call answers ENOENT for a path whose directory is not there, which
 * is the honest answer of this system (the shell of the kernel says the same
 * about its own `mkdir`).
 */
#include "ulibc.h"

int main(int argc, char *const argv[], char *const envp[])
{
    int status = 0;
    int i;

    (void)envp;

    if (argc < 2) {
        static const char usage[] = "usage: mkdir <directory>...\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    for (i = 1; i < argc; i++) {
        int rc = mkdir(argv[i]);

        if (rc < 0) {
            printf("mkdir: %s: error %d\n", argv[i], -rc);
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
