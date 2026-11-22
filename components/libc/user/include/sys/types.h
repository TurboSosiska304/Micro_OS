/*
 * Micro OS — the types of a POSIX header, for a ported program.
 *
 * The system has one width on both machines: the offsets of a file and the
 * results of read and write are long. The header exists because zlib and
 * other portable sources include it; it declares the same types as `ulibc.h`
 * and adds nothing. The guard is shared with `<unistd.h>`, so the two may be
 * included in either order and the types are declared once.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_TYPES_H
#define MICRO_OS_LIBC_USER_SYS_TYPES_H

#include <stddef.h>

#ifndef MICRO_OS_LIBC_USER_TYPES_DEFINED
#define MICRO_OS_LIBC_USER_TYPES_DEFINED
typedef long ssize_t;
typedef long off_t;
typedef long long off64_t;
#endif

#endif /* MICRO_OS_LIBC_USER_SYS_TYPES_H */
