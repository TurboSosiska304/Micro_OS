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
 * The size of the buffer a source of the C library asks for. There is no
 * buffer behind a stream of this system (every call reaches the kernel), but
 * a portable source writes `char buff[BUFSIZ]` on its stack and the number
 * has to be a sane one: 8 KiB is the block a file is read in.
 */
#ifndef BUFSIZ
#define BUFSIZ 8192
#endif

/* The modes of `setvbuf`. A stream of this system has no buffer, so the three
 * are accepted and none of them changes anything. */
#ifndef _IOFBF
#define _IOFBF 0
#endif
#ifndef _IOLBF
#define _IOLBF 1
#endif
#ifndef _IONBF
#define _IONBF 2
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
