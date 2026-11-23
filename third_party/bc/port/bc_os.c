/*
 * Micro OS — the stat and the signal of a bc program.
 *
 * The release reads a file of `-f` with `open`, `fstat` and `read`: it takes
 * the size of the file from the descriptor and refuses a directory. The ABI
 * of this system has `stat` of a path and no `fstat` of a descriptor: the
 * size is the position of the end (the jump that `lseek` makes), and the
 * object is named a regular file. A directory given to `-f` is read as an
 * empty file and not as one - a debt of the shape of the call and not of bc
 * (`docs/debts.md`).
 *
 * The signal handler of the release is `sigaction`; this kernel has
 * `signal` and four signals. The wrapper keeps the shape for the release and
 * refuses the number the system does not know, which the release ignores.
 *
 * Only the program of the machine compiles this file. The host test uses the
 * C library of the host, where both calls are real.
 */
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int fstat(int fd, struct stat *info)
{
    long size;

    if (info == NULL) {
        errno = EINVAL;
        return -1;
    }

    memset(info, 0, sizeof(*info));

    /* The end of the file is its size, and the descriptor is left at the
     * start: the release reads the whole file after this call. */
    size = lseek(fd, 0, ULIBC_SEEK_END);
    if (size < 0) {
        return -1; /* errno is the error of the call */
    }
    if (lseek(fd, 0, ULIBC_SEEK_SET) < 0) {
        return -1;
    }

    info->st_mode = S_IFREG | 0644;
    info->st_size = size;

    return 0;
}

int sigaction(int number, const struct sigaction *action, struct sigaction *old)
{
    (void)old;

    if (action == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (signal(number, action->sa_handler) == SIG_ERR) {
        return -1; /* errno is the number of the refusal */
    }

    return 0;
}
