/*
 * Micro OS — the streams of a program.
 *
 * A stream is a descriptor of the VFS and the little state a caller reads
 * back: the end of the file and the error of the last call. There is no
 * buffer between the stream and the kernel on purpose -
 * the files of this system are small, the calls are cheap, and a buffer of
 * the host library would be a second allocator with a second policy.
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
#include <stdio.h>
#include <stdlib.h>

#include "file_private.h"

static struct ulibc_file g_stdin_s = {ULIBC_STDIN, 0, 0, -1, NULL, 0, 0, 0};
static struct ulibc_file g_stdout_s = {ULIBC_STDOUT, 0, 0, -1, NULL, 0, 0, 0};
static struct ulibc_file g_stderr_s = {ULIBC_STDERR, 0, 0, -1, NULL, 0, 0, 0};

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
    stream->pushback = -1;

    return stream;
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

    /* The byte of ungetc is the first byte of the read. */
    if (stream->pushback >= 0 && got < total) {
        *bytes++ = (unsigned char)stream->pushback;
        stream->pushback = -1;
        got++;
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
        long rc = read(stream->fd, bytes + got, total - got);

        if (rc < 0) {
            stream->error = 1;
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

    if (stream == NULL) {
        errno = EINVAL;
        return EOF;
    }

    if (stream->pushback >= 0) {
        int byte_back = stream->pushback;

        stream->pushback = -1;
        return byte_back;
    }

    if (fread(&byte, 1, 1, stream) != 1) {
        return EOF;
    }

    return (int)byte;
}

/*
 * The one byte of look-ahead. A stream of this library has no buffer of its
 * own, so the standard's promise of one character is kept in the state of
 * the stream; a second call before a read is the error of the standard, and a
 * seek forgets it.
 */
int ungetc(int byte, FILE *stream)
{
    if (stream == NULL || byte == EOF) {
        errno = EINVAL;
        return EOF;
    }
    if (stream->pushback >= 0) {
        errno = EAGAIN;
        return EOF;
    }

    stream->pushback = (unsigned char)byte;
    stream->eof = 0;

    return (unsigned char)byte;
}

int getc(FILE *stream)
{
    return fgetc(stream);
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
        stream->pushback = -1;
        return 0;
    }

    stream->pushback = -1;
    rc = lseek(stream->fd, offset, whence);
    if (rc < 0) {
        stream->error = 1;
        return -1;
    }

    stream->eof = 0;
    stream->pushback = -1;
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
    }

    return rc;
}

void rewind(FILE *stream)
{
    (void)fseek(stream, 0, ULIBC_SEEK_SET);
    clearerr(stream);
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
    stream->pushback = -1;
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
 * The buffering of a stream. There is no buffer between a stream and the
 * kernel of this system: every write is a call and every read is the bytes
 * that are there, so the request is accepted and changes nothing.
 */
int setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    (void)stream;
    (void)buffer;
    (void)mode;
    (void)size;

    return 0;
}
