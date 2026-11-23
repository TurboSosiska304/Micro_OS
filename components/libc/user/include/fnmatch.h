/*
 * Micro OS — the pattern of a name, for `find -name`.
 *
 * The grammar is the one of POSIX `fnmatch`: `*` matches any run of bytes,
 * `?` one byte, `[...]` a set with ranges and `!`/`^`, and `\` quotes the next
 * byte. The flags this system answers are `FNM_NOESCAPE` and `FNM_PATHNAME`
 * (`*` does not cross a `/`); a period is not special in this grammar, so
 * `FNM_PERIOD` is accepted and does nothing, as the standard allows a
 * machine without the convention.
 */
#ifndef MICRO_OS_LIBC_USER_FNMATCH_H
#define MICRO_OS_LIBC_USER_FNMATCH_H

#define FNM_NOMATCH 1
#define FNM_PATHNAME (1 << 0)
#define FNM_NOESCAPE (1 << 1)
#define FNM_PERIOD (1 << 2)

int fnmatch(const char *pattern, const char *text, int flags);

#endif /* MICRO_OS_LIBC_USER_FNMATCH_H */
