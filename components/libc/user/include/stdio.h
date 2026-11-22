/*
 * Micro OS — the streams and the formatter of a program.
 *
 * A stream is a descriptor and a little state, not a buffer of the host: the
 * library of a program has no read-ahead and no lock, and every call reaches
 * the kernel through the door. `printf` and `puts` are the console (descriptor
 * 1); `fopen` and the `f*` functions are the files of the VFS.
 */
#ifndef MICRO_OS_LIBC_USER_STDIO_H
#define MICRO_OS_LIBC_USER_STDIO_H

#include "ulibc.h"

#ifndef EOF
#define EOF (-1)
#endif

/*
 * zlib's zconf.h defines the three numbers when the platform does not, and
 * it may have been read before this header: a second definition of the same
 * number is a warning that this build treats as an error.
 */
#ifndef SEEK_SET
#define SEEK_SET ULIBC_SEEK_SET
#endif
#ifndef SEEK_CUR
#define SEEK_CUR ULIBC_SEEK_CUR
#endif
#ifndef SEEK_END
#define SEEK_END ULIBC_SEEK_END
#endif

#endif /* MICRO_OS_LIBC_USER_STDIO_H */
