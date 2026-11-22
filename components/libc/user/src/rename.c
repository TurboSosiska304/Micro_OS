/*
 * Micro OS — the name of an object moved in its volume.
 *
 * The call is KSYS_RENAME, which the kernel table has had since ADR 0024 and
 * which the library of a program did not wrap before SQLite asked: a journal
 * and a temporary file are renamed by the shell of a database, and the
 * emulation of truncation in the port of SQLite is a copy and a rename. The
 * library resolves the two paths against the directory of the program, the
 * way `open` does.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdio.h>

#include "ulibc_internal.h"

int rename(const char *from, const char *to)
{
    const char *resolved_from = __ulibc_path(from);
    const char *resolved_to = __ulibc_path(to);
    long rc;

    if (resolved_from == NULL || resolved_to == NULL) {
        return -1;
    }

    rc = __ulibc_syscall6(KSYS_RENAME, (long)(uintptr_t)resolved_from, (long)(uintptr_t)resolved_to,
                          0, 0, 0, 0);
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }

    return 0;
}
