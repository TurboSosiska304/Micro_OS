/*
 * Micro OS — the streams of a program.
 *
 * A stream is a descriptor of the VFS and the little state a caller reads
 * back: the end of the file and the error of the last call. A block of the
 * stream holds the bytes of the descriptor that a `fgetc` has not asked for:
 * without it a line reader of the release (the `grep` of sbase over a file of
 * a few hundred kilobytes) is one system call per byte, which the machine
 * feels. The block lives in the object of the stream and not in the arena, so
 * a program of four streams has no allocation to make.
 *
 * A stream that is not a file is a block of memory: `fmemopen` gives a source
 * the stream it asks for over a word of its own, which is how `grep` reads a
 * pattern written on the command line, and the same object carries the one
 * byte of `ungetc`. The memory is the caller's, never freed by `fclose`.
 *
 * The binary mode of `fopen` is accepted and ignored: no file of the system
 * translates a byte, and a portable program asks for "rb" so that a machine
 * that does translate bytes does not change its data. The conversion of a
 * newline happens in the console of the port, not here.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdarg.h>

#include "ulibc_internal.h"
#include <stdio.h>
#include <stdlib.h>

struct ulibc_file {
    int fd;
    int eof;
    int error;
    unsigned char *data; /* the block of a stream of memory, or NULL */
    size_t size;
    size_t position;
    int memory;
    int pushed; /* the byte of ungetc, or -1 */
    /* The bytes read from the descriptor and not yet given to the caller. */
    unsigned char chunk[BUFSIZ];
    size_t chunk_at;
    size_t chunk_len;
};

static struct ulibc_file g_stdin_s = {ULIBC_STDIN, 0, 0, NULL, 0, 0, 0, -1, {0}, 0, 0};
static struct ulibc_file g_stdout_s = {ULIBC_STDOUT, 0, 0, NULL, 0, 0, 0, -1, {0}, 0, 0};
static struct ulibc_file g_stderr_s = {ULIBC_STDERR, 0, 0, NULL, 0, 0, 0, -1, {0}, 0, 0};

FILE *stdin = &g_stdin_s;
FILE *stdout = &g_stdout_s;
FILE *stderr = &g_stderr_s;

/* A stream over a descriptor, for `fopen` and `fdopen`. */
static FILE *stream_new(int fd)
{
    FILE *stream = (FILE *)malloc(sizeof(*stream));

    if (stream == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    stream->fd = fd;
    stream->eof = 0;
    stream->error = 0;
    stream->data = NULL;
    stream->size = 0;
    stream->position = 0;
    stream->memory = 0;
    stream->pushed = -1;
    stream->chunk_at = 0;
    stream->chunk_len = 0;

    return stream;
}

/*
 * Fill the block of the stream from its descriptor. A pipe of this system
 * answers EAGAIN while its producer is there (ADR 0014), which is not the end
 * of the stream: the call waits the condition out. The answer is 1 when bytes
 * arrived, 0 at the end of the file and -1 on an error.
 */
static int stream_fill(FILE *stream)
{
    long rc;

    stream->chunk_at = 0;
    stream->chunk_len = 0;
    if (stream->eof) {
        return 0;
    }

    do {
        rc = read(stream->fd, stream->chunk, sizeof(stream->chunk));
    } while (rc < 0 && errno == EAGAIN);

    if (rc < 0) {
        stream->error = 1;
        return -1;
    }
    if (rc == 0) {
        stream->eof = 1;
        return 0;
    }

    stream->chunk_len = (size_t)rc;
    return 1;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags = 0;
    int fd;
    FILE *stream;

    if (path == NULL || mode == NULL) {
        errno = EINVAL;
        return NULL;
    }

    switch (mode[0]) {
    case 'r':
        flags = ULIBC_O_RDONLY;
        break;
    case 'w':
        flags = ULIBC_O_WRONLY | ULIBC_O_CREAT | ULIBC_O_TRUNC;
        break;
    case 'a':
        flags = ULIBC_O_WRONLY | ULIBC_O_CREAT | ULIBC_O_APPEND;
        break;
    default:
        errno = EINVAL;
        return NULL;
    }

    for (const char *letter = mode; *letter != '\0'; letter++) {
        if (*letter == '+') {
            flags = (flags & ~3) | ULIBC_O_RDWR;
        }
    }

    fd = open(path, flags);
    if (fd < 0) {
        return NULL; /* open() set errno */
    }

    stream = stream_new(fd);
    if (stream == NULL) {
        (void)close(fd);
        return NULL;
    }

    return stream;
}

FILE *fdopen(int fd, const char *mode)
{
    (void)mode;

    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }

    return stream_new(fd);
}

/*
 * A stream over the block of the caller. The mode is the one of `fopen`; the
 * block does not grow, and a write past its end is the error of a full file.
 */
FILE *fmemopen(void *buffer, size_t size, const char *mode)
{
    FILE *stream;

    if (buffer == NULL || mode == NULL || mode[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    if (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a') {
        errno = EINVAL;
        return NULL;
    }

    stream = stream_new(-1);
    if (stream == NULL) {
        return NULL;
    }

    stream->data = (unsigned char *)buffer;
    stream->size = size;
    stream->position = 0;
    stream->memory = 1;

    return stream;
}

int fclose(FILE *stream)
{
    int rc = 0;

    if (stream == NULL) {
        errno = EINVAL;
        return EOF;
    }

    if (stream == stdin || stream == stdout || stream == stderr) {
        return 0; /* the console belongs to the task, not to this stream */
    }

    if (!stream->memory) {
        rc = close(stream->fd);
    }
    free(stream);

    if (rc < 0) {
        return EOF; /* close() set errno */
    }

    return 0;
}

int fileno(FILE *stream)
{
    if (stream == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (stream->memory) {
        errno = EBADF;
        return -1;
    }

    return stream->fd;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    unsigned char *bytes = (unsigned char *)buffer;
    size_t total;
    size_t got = 0;

    if (stream == NULL || buffer == NULL || size == 0 || count == 0) {
        return 0;
    }

    total = size * count;
    if (total / size != count) {
        errno = EINVAL;
        stream->error = 1;
        return 0;
    }

    /* The byte of ungetc is the first byte of the read. The pointer below is
     * the base of the block and every copy is at `bytes + got`: advancing the
     * base here as well would write the rest of the bytes one place late and
     * one byte past the end of the request. */
    if (stream->pushed >= 0 && got < total) {
        bytes[got++] = (unsigned char)stream->pushed;
        stream->pushed = -1;
    }

    if (stream->memory) {
        size_t left = stream->size - stream->position;
        size_t take = (total - got < left) ? total - got : left;

        memcpy(bytes, stream->data + stream->position, take);
        stream->position += take;
        got += take;
        if (got < total) {
            stream->eof = 1;
        }

        return got / size;
    }

    while (got < total) {
        size_t left = total - got;

        if (stream->chunk_at < stream->chunk_len) {
            size_t take = stream->chunk_len - stream->chunk_at;

            if (take > left) {
                take = left;
            }
            memcpy(bytes + got, stream->chunk + stream->chunk_at, take);
            stream->chunk_at += take;
            got += take;
            continue;
        }

        /* A request larger than the block is read into the caller's memory
         * and not through the block: a copy of the whole file is one read. */
        if (left >= sizeof(stream->chunk)) {
            long rc = read(stream->fd, bytes + got, left);

            if (rc < 0 && errno == EAGAIN) {
                continue;
            }
            if (rc < 0) {
                stream->error = 1;
                break;
            }
            if (rc == 0) {
                stream->eof = 1;
                break;
            }
            got += (size_t)rc;
            continue;
        }

        {
            int filled = stream_fill(stream);

            if (filled <= 0) {
                break;
            }
        }
    }

    return got / size;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t total;
    size_t put = 0;

    if (stream == NULL || buffer == NULL || size == 0 || count == 0) {
        return 0;
    }

    total = size * count;
    if (total / size != count) {
        errno = EINVAL;
        stream->error = 1;
        return 0;
    }

    if (stream->memory) {
        size_t left = stream->size - stream->position;
        size_t take = (total < left) ? total : left;

        if (take > 0) {
            memcpy(stream->data + stream->position, bytes, take);
            stream->position += take;
        }
        if (take < total) {
            stream->error = 1;
            errno = ENOSPC;
        }

        return take / size;
    }

    while (put < total) {
        long rc = write(stream->fd, bytes + put, total - put);

        if (rc < 0) {
            stream->error = 1;
            break;
        }
        if (rc == 0) {
            stream->error = 1;
            break;
        }
        put += (size_t)rc;
    }

    return put / size;
}

int fgetc(FILE *stream)
{
    unsigned char byte;
    size_t got;

    if (stream == NULL) {
        errno = EINVAL;
        return EOF;
    }

    if (stream->pushed >= 0) {
        int value = stream->pushed;

        stream->pushed = -1;
        return value;
    }

    got = fread(&byte, 1, 1, stream);
    if (got != 1) {
        return EOF;
    }

    return (int)byte;
}

int ungetc(int byte, FILE *stream)
{
    if (stream == NULL || byte == EOF) {
        errno = EINVAL;
        return EOF;
    }
    if (stream->pushed >= 0) {
        errno = EAGAIN;
        return EOF;
    }

    stream->pushed = (unsigned char)byte;
    stream->eof = 0;

    return (unsigned char)byte;
}

int fputc(int byte, FILE *stream)
{
    unsigned char value = (unsigned char)byte;

    if (stream == NULL) {
        errno = EINVAL;
        return EOF;
    }

    if (fwrite(&value, 1, 1, stream) != 1) {
        return EOF;
    }

    return value;
}

char *fgets(char *buffer, int size, FILE *stream)
{
    int used = 0;

    if (buffer == NULL || size <= 0 || stream == NULL) {
        errno = EINVAL;
        return NULL;
    }

    while (used + 1 < size) {
        int byte = fgetc(stream);

        if (byte == EOF) {
            break;
        }

        buffer[used++] = (char)byte;
        if (byte == '\n') {
            break;
        }
    }

    if (used == 0) {
        return NULL;
    }

    buffer[used] = '\0';
    return buffer;
}

int fputs(const char *text, FILE *stream)
{
    size_t length;

    if (text == NULL || stream == NULL) {
        errno = EINVAL;
        return EOF;
    }

    length = strlen(text);
    if (fwrite(text, 1, length, stream) != length) {
        return EOF;
    }

    return 0;
}

int feof(FILE *stream)
{
    return (stream != NULL) ? stream->eof : 0;
}

int ferror(FILE *stream)
{
    return (stream != NULL) ? stream->error : 0;
}

void clearerr(FILE *stream)
{
    if (stream != NULL) {
        stream->eof = 0;
        stream->error = 0;
    }
}

int fflush(FILE *stream)
{
    /* Every write already reached the kernel: there is nothing to flush. */
    (void)stream;
    return 0;
}

int fseek(FILE *stream, long offset, int whence)
{
    long rc;

    if (stream == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (stream->memory) {
        long target;

        if (whence == ULIBC_SEEK_SET) {
            target = offset;
        } else if (whence == ULIBC_SEEK_CUR) {
            target = (long)stream->position + offset;
        } else if (whence == ULIBC_SEEK_END) {
            target = (long)stream->size + offset;
        } else {
            errno = EINVAL;
            return -1;
        }
        if (target < 0 || (size_t)target > stream->size) {
            errno = EINVAL;
            stream->error = 1;
            return -1;
        }

        stream->position = (size_t)target;
        stream->eof = 0;
        stream->pushed = -1;
        return 0;
    }

    /*
     * The position of a stream is the one of the descriptor less the bytes of
     * the block the caller has not read. A seek from the position of the
     * stream has to take that off the offset: the descriptor stands at the end
     * of the block, and `fseek(f, -12, SEEK_CUR)` of a caller that read a
     * frame header of six bytes out of a block of eighteen asks for six and
     * not for eighteen less twelve from the end of the file.
     */
    if (whence == ULIBC_SEEK_CUR) {
        offset -= (long)(stream->chunk_len - stream->chunk_at);
    }
    stream->pushed = -1;
    stream->chunk_at = 0;
    stream->chunk_len = 0;
    rc = lseek(stream->fd, offset, whence);
    if (rc < 0) {
        stream->error = 1;
        return -1;
    }

    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream)
{
    long rc;

    if (stream == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (stream->memory) {
        return (long)stream->position;
    }

    rc = lseek(stream->fd, 0, ULIBC_SEEK_CUR);
    if (rc < 0) {
        stream->error = 1;
        return rc;
    }

    /* The bytes of the block are ahead of the descriptor: the position of the
     * stream is the one of the descriptor less what is still in the block. */
    return rc - (long)(stream->chunk_len - stream->chunk_at);
}

void rewind(FILE *stream)
{
    (void)fseek(stream, 0, ULIBC_SEEK_SET);
    clearerr(stream);
}

/*
 * The formatter of a stream. The text is built by `src/stdio.c` (the formatter
 * of `printf`) and written to the descriptor of the stream in one call; the
 * call lives here and not there, because a `FILE` is private to this file and
 * a program that never opens one (`cat`) does not link the streams at all.
 */
int vfprintf(FILE *stream, const char *format, va_list args)
{
    int fd;

    if (stream == NULL || format == NULL) {
        errno = EINVAL;
        return -1;
    }

    fd = fileno(stream);
    if (fd < 0) {
        return -1;
    }

    return __ulibc_format_fd(fd, format, args);
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vfprintf(stream, format, args);
    va_end(args);

    return length;
}

/*
 * The character of `fgetc` under the name a source of a Unix-like system uses
 * when it walks a file byte by byte. The macro of the standard is a function
 * here: the library has one entry and no second copy in every file that
 * includes `<stdio.h>`.
 */
int getc(FILE *stream)
{
    return fgetc(stream);
}

/*
 * The removal of a file, in the POSIX shape: 0 or -1 with `errno`. The call
 * of the ABI answers the negative number, and a portable source asks the
 * library for the other convention.
 */
int remove(const char *path)
{
    int rc = unlink(path);

    if (rc < 0) {
        errno = -rc;
        return -1;
    }

    return 0;
}

/*
 * The same path opened again on the same stream. The release of Lua asks for
 * it to reopen a file with a binary mode after it read the signature of a
 * compiled chunk; no file of this system translates a byte, so the mode is
 * read and has no other effect, and the answer is the stream itself.
 */
FILE *freopen(const char *path, const char *mode, FILE *stream)
{
    FILE *opened;

    if (stream == NULL) {
        errno = EINVAL;
        return NULL;
    }

    opened = fopen(path, mode);
    if (opened == NULL) {
        return NULL;
    }

    if (stream != stdin && stream != stdout && stream != stderr) {
        (void)close(stream->fd);
    }
    stream->fd = opened->fd;
    stream->eof = 0;
    stream->error = 0;
    stream->memory = 0;
    stream->pushed = -1;
    stream->chunk_at = 0;
    stream->chunk_len = 0;
    free(opened);

    return stream;
}

/*
 * A file with no name. The system has no directory of temporary files - the
 * volume of a machine is its own MicroFS - so the call answers the failure
 * of the platform and the library of Lua reports it as the error of
 * `io.tmpfile`. It is a debt of the platform, not a file that leaks.
 */
FILE *tmpfile(void)
{
    errno = ENOSYS;

    return NULL;
}

/*
 * The buffering of a stream. A write goes to the kernel in the call of
 * `fwrite` and a read keeps its own block, so the request of a caller is the
 * state the library already has: the call is accepted and changes nothing.
 */
int setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    (void)stream;
    (void)buffer;
    (void)mode;
    (void)size;

    return 0;
}

/*
 * The byte of the standard input, which is the call a program of a Unix-like
 * system makes when it asks a question of a person (`zstd` asks whether to
 * overwrite a file, and a script of this system never sees the question).
 */
int getchar(void)
{
    return fgetc(stdin);
}

/*
 * The error of the last call with the word of the caller in front of it, on
 * the descriptor of the errors. A program of a Unix-like system prints a
 * failure this way (`zstd` says `perror("zstd:util")` where a line of a file
 * could not be read); the text goes to the stream and the answer is 0, or EOF
 * when the write failed.
 */
int perror(const char *text)
{
    int rc;

    if (text != NULL && text[0] != '\0') {
        rc = fprintf(stderr, "%s: %s\n", text, strerror(errno));
    } else {
        rc = fprintf(stderr, "%s\n", strerror(errno));
    }

    return (rc < 0) ? EOF : 0;
}
