/*
 * Micro OS — the parameters of the machine, for a portable source.
 *
 * A library that came from BSD includes <sys/param.h> for `MIN`, `MAX` and
 * `howmany`, which are three expressions of the C preprocessor and not calls
 * of the kernel. The file is small on purpose: the port of a program should
 * not find a second definition of a structure here.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_PARAM_H
#define MICRO_OS_LIBC_USER_SYS_PARAM_H

#include <sys/types.h>

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* The number of units of `y` needed to hold `x`, rounded up. */
#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif
#ifndef roundup
#define roundup(x, y) (howmany((x), (y)) * (y))
#endif

#ifndef MAXPATHLEN
#define MAXPATHLEN 128
#endif
#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 64
#endif

#endif /* MICRO_OS_LIBC_USER_SYS_PARAM_H */
