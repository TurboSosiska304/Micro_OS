/*
 * Micro OS — the canonical name of a path.
 *
 * The canonical name of a path is the absolute path that names the same
 * object with no `.`, no `..` and no doubled slash in it. This system has no
 * symbolic link, so that name is the fold the library already does for every
 * call of a path (`src/path.c`, `__ulibc_path`) and one question to the
 * filesystem: the object has to exist. The fold is done against the directory
 * of the program, and the answer is always absolute.
 *
 * The VFS resolves a path one component at a time and keeps no link, so the
 * canonical name of a path is the fold that the library does for every call
 * of a path (`src/path.c`, `__ulibc_path`) - a component `.` dropped, `..`
 * popping the one before it, the doubled slashes away - and one question to
 * the filesystem: the object has to exist. A path that does not exist is
 * `ENOENT`, an empty one is `ENOENT` too (POSIX: there is no name), and a
 * name over `PATH_MAX` is `ENAMETOOLONG`, which the resolver has already
 * refused. The fold is lexically the answer because no link stands between
 * two names; a `..` after a regular file therefore names its directory here
 * where a machine with links would refuse the path.
 *
 * A buffer of the caller is `PATH_MAX` bytes by the contract of POSIX, and a
 * NULL buffer asks the arena for the answer; the caller frees that one.
 */
#include "ulibc_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *realpath(const char *path, char *buffer)
{
    const char *resolved;
    struct vfs_stat info;
    size_t length;
    char *out;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    resolved = __ulibc_path(path);
    if (resolved == NULL) {
        return NULL;
    }

    /*
     * The name has to exist. The call of the information is the plain stat of
     * the VFS (`KSYS_STAT`): the type and the answer are all this one needs,
     * and the statx of the moments would read a record for it.
     */
    {
        long answer = __ulibc_syscall6(KSYS_STAT, (long)(unsigned long)resolved,
                                       (long)(unsigned long)&info, 0, 0, 0, 0);

        if (answer < 0) {
            errno = (int)-answer;
            return NULL;
        }
    }

    length = strlen(resolved);
    if (buffer != NULL) {
        memcpy(buffer, resolved, length + 1);
        return buffer;
    }

    out = (char *)malloc(length + 1);
    if (out == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(out, resolved, length + 1);

    return out;
}
