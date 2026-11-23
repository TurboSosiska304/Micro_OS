/*
 * Micro OS — the two halves of a path, for `find`.
 *
 * `basename` and `dirname` may change the buffer they are given, as POSIX
 * allows; a source that needs its path afterwards copies it first. There are
 * no links and no `..` in a path of this system, so the two functions only
 * cut at the last slash.
 */
#ifndef MICRO_OS_LIBC_USER_LIBGEN_H
#define MICRO_OS_LIBC_USER_LIBGEN_H

char *basename(char *path);
char *dirname(char *path);

#endif /* MICRO_OS_LIBC_USER_LIBGEN_H */
