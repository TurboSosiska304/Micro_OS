/*
 * Micro OS — truncate, a file cut by the call of the kernel.
 *
 * The program opens the file for writing, asks the kernel to cut it to the
 * size of the line and prints the size that the file has afterwards. It is
 * the proof of `ftruncate` on a machine with a real filesystem: the host
 * tests cover MicroFS and ext2, and the smoke test runs this file of /bin
 * against the volume the boot mounted.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *const argv[], char *const envp[])
{
    char *end = NULL;
    unsigned long long size;
    int fd;

    (void)envp;

    if (argc != 3) {
        static const char usage[] = "usage: truncate <size> <file>\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    size = strtoull(argv[1], &end, 10);
    if (end == NULL || *end != '\0') {
        printf("truncate: %s: not a size\n", argv[1]);
        return ULIBC_EXIT_FAILURE;
    }

    fd = open(argv[2], ULIBC_O_RDWR);
    if (fd < 0) {
        printf("truncate: %s: %s\n", argv[2], strerror(errno));
        return ULIBC_EXIT_FAILURE;
    }

    if (ftruncate(fd, (long long)size) != 0) {
        printf("truncate: %s: %s\n", argv[2], strerror(errno));
        (void)close(fd);
        return ULIBC_EXIT_FAILURE;
    }

    if (close(fd) != 0) {
        printf("truncate: %s: %s\n", argv[2], strerror(errno));
        return ULIBC_EXIT_FAILURE;
    }

    printf("truncate: %s is %llu bytes\n", argv[2], size);
    return ULIBC_EXIT_SUCCESS;
}
