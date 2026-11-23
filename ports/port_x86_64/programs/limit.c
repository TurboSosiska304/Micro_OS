/*
 * Micro OS — the memory limit of a task, from the program itself.
 *
 * `setrlimit(RLIMIT_AS, ...)` is the call 33 of the ABI (`setlimit`), and the
 * heap of a program is charged to it: a program may lower its own ceiling and
 * then it cannot have more memory than that, which is what the shell's
 * `ulimit -m` promises and what a program that starts its own children needs.
 *
 * The limits may only be lowered, so this program lowers first and asks after
 * it: the window of 800 by 600 pixels (1 920 000 bytes) is refused with
 * ENOMEM, a block of half a megabyte still fits, and the block that fits is
 * the proof that the refusal was the limit and not the heap.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#define WINDOW_BYTES (800u * 600u * 4u)
#define SMALL_BYTES (512u * 1024u)

int main(int argc, char *const argv[], char *const envp[])
{
    struct rlimit limit;
    void *block;

    (void)argc;
    (void)argv;
    (void)envp;

    if (getrlimit(RLIMIT_AS, &limit) != 0) {
        printf("limit: getrlimit: %d\n", errno);
        return ULIBC_EXIT_FAILURE;
    }

    printf("limit: the memory limit is %llu bytes\n", (unsigned long long)limit.rlim_cur);

    limit.rlim_cur = 1024u * 1024u;
    limit.rlim_max = 1024u * 1024u;

    if (setrlimit(RLIMIT_AS, &limit) != 0) {
        printf("limit: setrlimit: %d\n", errno);
        return ULIBC_EXIT_FAILURE;
    }

    if (getrlimit(RLIMIT_AS, &limit) == 0) {
        printf("limit: lowered to %llu bytes\n", (unsigned long long)limit.rlim_cur);
    }

    /* A raise of a limit is the refusal of the kernel: EPERM (1). */
    limit.rlim_cur = 4u * 1024u * 1024u;
    printf("limit: a raise says %d\n", setrlimit(RLIMIT_AS, &limit) == 0 ? 0 : errno);

    errno = 0;
    block = malloc(WINDOW_BYTES);
    if (block == NULL) {
        printf("limit: a window of %u bytes is refused with %d\n", (unsigned)WINDOW_BYTES, errno);
    } else {
        printf("limit: a window of %u bytes was taken\n", (unsigned)WINDOW_BYTES);
        free(block);
    }

    block = malloc(SMALL_BYTES);
    if (block == NULL) {
        printf("limit: a block of %u bytes is refused with %d\n", (unsigned)SMALL_BYTES, errno);
        return ULIBC_EXIT_FAILURE;
    }

    memset(block, 0x5A, SMALL_BYTES);
    printf("limit: a block of %u bytes is taken and ends with %u\n", (unsigned)SMALL_BYTES,
           (unsigned)((unsigned char *)block)[SMALL_BYTES - 1]);
    free(block);

    printf("limit: done\n");

    return ULIBC_EXIT_SUCCESS;
}
