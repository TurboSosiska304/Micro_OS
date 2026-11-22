/*
 * Micro OS — the streams and the formatter of a program.
 *
 * A stream is a descriptor and a little state, not a buffer of the host: the
 * library of a program has no read-ahead and no lock, and every call reaches
 * the kernel through the door. `printf` and `puts` are the console (descriptor
 * 1); `fopen` and the `f*` functions are the files of the VFS.
 *
 * The formatter is the one of `printf`: a conversion this header does not
 * name is not in it, and the sentence a program prints is the sentence of the
 * system. `fprintf` and `vfprintf` format into a block of the arena and write
 * it to the stream, so a line of any length leaves in one piece.
 */
#ifndef MICRO_OS_LIBC_USER_STDIO_H
#define MICRO_OS_LIBC_USER_STDIO_H

#include "ulibc.h"

#ifndef MICRO_OS_LIBC_USER_TYPES_DEFINED
#define MICRO_OS_LIBC_USER_TYPES_DEFINED
typedef long ssize_t;
typedef long off_t;
typedef long long off64_t;
#endif

#ifndef EOF
#define EOF (-1)
#endif

#ifndef BUFSIZ
#define BUFSIZ 512
#endif

/*
 * The longest name of a file, as the C library names it. The shell of SQLite
 * builds a name on the stack with this bound; a path of this system is 128
 * bytes, the same number the VFS takes.
 */
#ifndef FILENAME_MAX
#define FILENAME_MAX 128
#endif

/* The buffering of a stream. The streams of this library have no buffer, so
 * `setvbuf` accepts the request and does nothing; the names are here because
 * a portable source writes them. */
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

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

int sprintf(char *buffer, const char *format, ...);
int sscanf(const char *text, const char *format, ...);
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);

/*
 * The pipe of a command. This system can start a program (`execve`) but not
 * hand it a stream: there is no `popen` in the library, and the two calls
 * answer `ENOSYS`. A source that compiles against them can say so to its
 * caller (src/unsupported.c).
 */
FILE *popen(const char *command, const char *mode);
int pclose(FILE *stream);
/*
 * The name of an object of the volume moved: the two paths are resolved
 * against the directory of the program, and a rename replaces the file at
 * the destination (ADR 0024). POSIX declares it here, beside the streams.
 */
int rename(const char *from, const char *to);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list args);
int vprintf(const char *format, va_list args);
int putchar(int byte);
int getchar(void);
void perror(const char *text);

int fileno(FILE *stream);
FILE *fdopen(int fd, const char *mode);
FILE *fmemopen(void *buffer, size_t size, const char *mode);

int ungetc(int byte, FILE *stream);
ssize_t getline(char **line, size_t *capacity, FILE *stream);
ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *stream);

#define getc(stream) fgetc(stream)
#define putc(byte, stream) fputc((byte), (stream))

#endif /* MICRO_OS_LIBC_USER_STDIO_H */
