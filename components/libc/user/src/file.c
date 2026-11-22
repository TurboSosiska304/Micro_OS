/*
 * Micro OS — the streams of a program.
 *
 * A stream is a descriptor of the VFS and the little state a caller reads
 * back: the end of the file and the error of the last call. There is no
 * buffer between the stream and the kernel on purpose -
 * the files of this system are small, the calls are cheap, and a buffer of
 * the host library would be a second allocator with a second policy.
 *
 * The binary mode of `fopen` is accepted and ignored: no file of the system
 * translates a byte, and a portable program asks for "rb" so that a machine
 * that does translate bytes does not change its data. The conversion of a
 * newline happens in the console of the port, not here.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

struct ulibc_file {
    int fd;
    int eof;
    int error;
};

static struct ulibc_file g_stdin_s = {ULIBC_STDIN, 0, 0};
static struct ulibc_file g_stdout_s = {ULIBC_STDOUT, 0, 0};
static struct ulibc_file g_stderr_s = {ULIBC_STDERR, 0, 0};

FILE *stdin = &g_stdin_s;
FILE *stdout = &g_stdout_s;
FILE *stderr = &g_stderr_s;

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
        errno = -fd;
        return NULL;
    }

    stream = (FILE *)malloc(sizeof(*stream));
    if (stream == NULL) {
        (void)close(fd);
        errno = ENOMEM;
        return NULL;
    }

    stream->fd = fd;
    stream->eof = 0;
    stream->error = 0;

    return stream;
}

int fclose(FILE *stream)
{
    int rc;

    if (stream == NULL) {
        errno = EINVAL;
        return EOF;
    }

    if (stream == stdin || stream == stdout || stream == stderr) {
        return 0; /* the console belongs to the task, not to this stream */
    }

    rc = close(stream->fd);
    free(stream);

    if (rc < 0) {
        errno = -rc;
        return EOF;
    }

    return 0;
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

    while (got < total) {
        long rc = read(stream->fd, bytes + got, total - got);

        if (rc < 0) {
            stream->error = 1;
            errno = (int)-rc;
            break;
        }
        if (rc == 0) {
            stream->eof = 1;
            break;
        }
        got += (size_t)rc;
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

    while (put < total) {
        long rc = write(stream->fd, bytes + put, total - put);

        if (rc < 0) {
            stream->error = 1;
            errno = (int)-rc;
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

    if (stream == NULL) {
        errno = EINVAL;
        return EOF;
    }

    if (fread(&byte, 1, 1, stream) != 1) {
        return EOF;
    }

    return (int)byte;
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

    rc = lseek(stream->fd, offset, whence);
    if (rc < 0) {
        errno = (int)-rc;
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

    rc = lseek(stream->fd, 0, ULIBC_SEEK_CUR);
    if (rc < 0) {
        errno = (int)-rc;
        stream->error = 1;
    }

    return rc;
}

void rewind(FILE *stream)
{
    (void)fseek(stream, 0, ULIBC_SEEK_SET);
    clearerr(stream);
}
