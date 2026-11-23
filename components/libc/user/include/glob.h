/*
 * Micro OS — the names of a pattern, for a portable source.
 *
 * `glob` walks the tree a pattern names: each component of the path with a
 * `*`, a `?` or a `[` set is matched against the entries of the directory
 * above it with `fnmatch`, and a component without one is taken as the name
 * itself. The answer is the array of the names that exist, in the order of the
 * names of the directory or sorted, and `globfree` gives the array back.
 *
 * The flags this system answers are `GLOB_NOSORT`, `GLOB_NOCHECK` (a pattern
 * that matched nothing is the pattern itself), `GLOB_APPEND` (the matches go
 * after the ones of the previous call), `GLOB_MARK` (a directory carries a
 * trailing slash), `GLOB_DOOFFS` with `gl_offs` (the caller keeps a few
 * leading NULLs for its own words) and `GLOB_ERR` with the callback of the
 * caller. A name that begins with a dot is matched only by a pattern that
 * begins with a dot, which is the convention of a Unix-like system; the names
 * `.` and `..` are never the answer of a pattern.
 */
#ifndef MICRO_OS_LIBC_USER_GLOB_H
#define MICRO_OS_LIBC_USER_GLOB_H

#include "ulibc.h"

typedef struct {
    size_t gl_pathc; /* the number of the names, without the NULLs of gl_offs */
    char **gl_pathv; /* the array: gl_offs NULLs, the names, and a NULL */
    size_t gl_offs;  /* the NULLs the caller wants in front of the names */
} glob_t;

#define GLOB_ERR 0x0001
#define GLOB_MARK 0x0002
#define GLOB_NOSORT 0x0004
#define GLOB_DOOFFS 0x0008
#define GLOB_NOCHECK 0x0010
#define GLOB_NOESCAPE 0x0020
#define GLOB_APPEND 0x0040

#define GLOB_ABORTED 1
#define GLOB_NOMATCH 2
#define GLOB_NOSPACE 3

int glob(const char *pattern, int flags, int (*error)(const char *path, int code), glob_t *result);
void globfree(glob_t *result);

#endif /* MICRO_OS_LIBC_USER_GLOB_H */
