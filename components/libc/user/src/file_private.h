/*
 * Micro OS — the state of a stream, between the two files that keep it.
 *
 * A stream of the library is a descriptor, three flags and a block of memory,
 * and the file is private on purpose: a program uses `FILE *` and the
 * functions, not the fields. The formatter of `fprintf` writes through the
 * block of a stream and lives in `src/stdio.c`; the streams themselves live
 * in `src/file.c`. This header is the seam between them, not a part of the
 * library a program includes.
 */
#ifndef MICRO_OS_LIBC_USER_FILE_PRIVATE_H
#define MICRO_OS_LIBC_USER_FILE_PRIVATE_H

#include "ulibc.h"

/*
 * The default block of a stream: the unit of every read and write that does
 * not come from the caller. 4 KiB is small against the 64 KiB a pipe of the
 * machine holds and large against a line; it is the grain of the kernel's
 * block device, and the block of one stream lives in the arena of the
 * program, not in the object of the stream.
 */
#define ULIBC_STREAM_BLOCK 4096u

struct ulibc_file {
    int fd;
    int eof;
    int error;
    /* One byte of look-ahead: `ungetc` puts back the character a reader of a
     * number did not want, and `fgetc` answers it before the next read. The
     * standard promises one byte and no more. */
    int pushback;
    /*
     * The block of the stream. It is the read-ahead of the descriptor (the
     * bytes between `at` and `len`) and the bytes written and not yet given
     * to the kernel (the first `used` bytes); the two directions never share
     * the block at once, which is the rule of an update stream of C.
     *
     * The memory is the arena of the program (`malloc`) or the block of the
     * caller of `setvbuf`, and `own_block` says which: `fclose` gives back
     * only what the library took. A stream that is not buffered borrows the
     * one byte `single`, so there is no second case below it.
     */
    unsigned char *block;
    size_t capacity;
    unsigned char single;
    size_t at;
    size_t len;
    size_t used;
    int mode;      /* _IOFBF, _IOLBF or _IONBF; negative until it is chosen */
    int own_block; /* the block came from malloc and goes back to it */
    int listed;    /* the stream is on the list of `fflush(NULL)` and exit */
    struct ulibc_file *next;
    /* The block of a stream of memory (`fmemopen`), or NULL. The memory is
     * the caller's, never freed by `fclose`, and the position is the index of
     * the next byte. */
    unsigned char *data;
    size_t size;
    size_t position;
    int memory;
};

/*
 * The write of the library to a stream: `size` bytes, the whole of them or
 * the answer is short and the error flag of the stream is set. The formatter
 * of `fprintf` uses it so that its text goes through the same block as
 * `fwrite`.
 */
size_t __ulibc_stream_write(FILE *stream, const void *data, size_t size);

/*
 * The console of a program that does not link the streams: `printf` and the
 * friends of `src/stdio.c` write the bytes to descriptor 1, and a program
 * that links `src/file.c` gets the buffered answer of `stdout` instead. The
 * two definitions are the same symbol, one of them weak.
 */
int __ulibc_stream_write_stdout(const void *data, size_t size);

/* Flush every stream; the other half of `exit` and `fclose`. */
void __ulibc_stream_flush_all(void);

#endif /* MICRO_OS_LIBC_USER_FILE_PRIVATE_H */
