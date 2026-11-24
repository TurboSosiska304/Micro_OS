/*
 * Micro OS — the calls of a Unix-like system that this one does not have.
 *
 * The names are here because a portable source compiles against them (the
 * shell of SQLite is one) and a link of the image has to resolve every one.
 * Each answers `ENOSYS` and changes nothing: a symbolic link is not an object
 * of MicroFS, the timestamps of a record are written by the filesystem and
 * not set by a call, and the stream of a command has no pipe to build. The
 * caller's own fallback is the honest one - SQLite's archive code refuses the
 * clone - and the debt is a row in docs/debts.md, not a fake success.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

int symlink(const char *target, const char *path)
{
    (void)target;
    (void)path;

    errno = ENOSYS;
    return -1;
}

int utimes(const char *path, const struct timeval times[2])
{
    (void)path;
    (void)times;

    errno = ENOSYS;
    return -1;
}

/*
 * There is no name of a machine in the ABI: the host name of `/etc` belongs
 * to the settings of the platform (`components/config`), not to a call of a
 * program. The shell of SQLite falls back to its own word for the prompt.
 */
int gethostname(char *name, size_t size)
{
    (void)name;
    (void)size;

    errno = ENOSYS;
    return -1;
}
