/*
 * Micro OS — the limits of the C library, for a portable source.
 *
 * The header keeps the compiler's own limits (a byte is eight bits, a long is
 * at least four) and adds the two names a portable source uses for a path and
 * a name. Both are the numbers of the VFS, so a buffer of a program is the
 * size the kernel will accept and not a guess: a path of this system is 127
 * bytes plus the terminator, a name 31 plus it.
 */
#ifndef MICRO_OS_LIBC_USER_LIMITS_H
#define MICRO_OS_LIBC_USER_LIMITS_H

#if defined(__GNUC__)
#include_next <limits.h>
#endif

#include "kernel/ksys.h"

#ifndef PATH_MAX
#define PATH_MAX (KSYS_PATH_MAX + 1)
#endif
#ifndef NAME_MAX
#define NAME_MAX 32
#endif
#ifndef SSIZE_MAX
#define SSIZE_MAX (__LONG_MAX__)
#endif
#ifndef CHAR_BIT
#define CHAR_BIT 8
#endif

#endif /* MICRO_OS_LIBC_USER_LIMITS_H */
