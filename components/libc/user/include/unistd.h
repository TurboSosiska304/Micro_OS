/*
 * Micro OS — the calls of a program under the names POSIX gives them.
 *
 * The prototypes are the ones of `ulibc.h`; the file adds the two names a
 * portable source expects and the numbers of `lseek`. The types of a file
 * are declared here as well as in `<sys/types.h>`, under one guard, because
 * a source may include either header first and the two are one set.
 */
#ifndef MICRO_OS_LIBC_USER_UNISTD_H
#define MICRO_OS_LIBC_USER_UNISTD_H

#include "ulibc.h"

#ifndef MICRO_OS_LIBC_USER_TYPES_DEFINED
#define MICRO_OS_LIBC_USER_TYPES_DEFINED
typedef long ssize_t;
typedef long off_t;
typedef long long off64_t;
#endif

#define STDIN_FILENO ULIBC_STDIN
#define STDOUT_FILENO ULIBC_STDOUT
#define STDERR_FILENO ULIBC_STDERR

#endif /* MICRO_OS_LIBC_USER_UNISTD_H */
