/*
 * Micro OS — the state of a stream, between the two files that keep it.
 *
 * A stream of the library is a descriptor and three flags, and the file is
 * private on purpose: a program uses `FILE *` and the functions, not the
 * fields. The formatter of `fprintf` writes to the descriptor of a stream and
 * lives in `src/stdio.c`; the streams themselves live in `src/file.c`. This
 * header is the seam between them, not a part of the library a program
 * includes.
 */
#ifndef MICRO_OS_LIBC_USER_FILE_PRIVATE_H
#define MICRO_OS_LIBC_USER_FILE_PRIVATE_H

#include "ulibc.h"

struct ulibc_file {
    int fd;
    int eof;
    int error;
    /* One byte of look-ahead: `ungetc` puts back the character a reader of a
     * number did not want, and `fgetc` answers it before the next read. The
     * standard promises one byte and no more. */
    int pushback;
    /* The block of a stream of memory (`fmemopen`), or NULL. The memory is
     * the caller's, never freed by `fclose`, and the position is the index of
     * the next byte. */
    unsigned char *data;
    size_t size;
    size_t position;
    int memory;
};

#endif /* MICRO_OS_LIBC_USER_FILE_PRIVATE_H */
