/*
 * Micro OS — the streams of a program.
 *
 * A stream is a descriptor of the VFS and the little state a caller reads
 * back: the end of the file and the error of the last call. Between the
 * caller and the descriptor there is one block of memory, and its size is
 * the whole difference between a machine that talks to its kernel in blocks
 * and one that talks to it in bytes: without the block a line reader of the
 * release is one system call per byte and a filter is one call per line.
 *
 * The block comes from the arena of the program, not from the object of the
 * stream, and not from the bss: a stream that never buffers costs nothing,
 * and `setvbuf` may hand the stream a block of the caller instead. The mode
 * is chosen when the stream is first used - a terminal is line buffered, a
 * file and a pipe are block buffered, the error descriptor is not buffered
 * at all - and `setvbuf` overrides it. `fflush`, `fclose` and the end of the
 * program give the bytes to the kernel; `_exit` does not, as POSIX says.
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
#include <string.h>
#include <unistd.h>

#include "file_private.h"

/*
 * The most one call of the system moves (KSYS_MAX_IO of the ABI). A read of
 * the caller larger than this is broken into pieces here, because the door
 * refuses a buffer above the bound and a caller of `fread` asks for the
 * whole file and not for the block of the kernel.
 */
#define STREAM_IO_MAX 65536u

static struct ulibc_file g_stdin_s = {.fd = ULIBC_STDIN, .pushback = -1, .mode = -1, .single = 0};
static struct ulibc_file g_stdout_s = {.fd = ULIBC_STDOUT, .pushback = -1, .mode = -1, .single = 0};
static struct ulibc_file g_stderr_s = {.fd = ULIBC_STDERR, .pushback = -1, .mode = -1, .single = 0};

FILE *stdin = &g_stdin_s;
FILE *stdout = &g_stdout_s;
FILE *stderr = &g_stderr_s;

/* The streams that `fflush(NULL)` and the end of the program walk. */
static struct ulibc_file *g_streams;

static void stream_list_add(FILE *stream)
{
    if (stream->listed) {
        return;
    }

    stream->next = g_streams;
    g_streams = stream;
    stream->listed = 1;
}

static void stream_list_remove(FILE *stream)
{
    struct ulibc_file **at = &g_streams;

    while (*at != NULL) {
        if (*at == stream) {
            *at = stream->next;
            stream->next = NULL;
            stream->listed = 0;
            return;
        }
        at = &(*at)->next;
    }
}

static int stream_is_standard(const FILE *stream)
{
    return stream == stdin || stream == stdout || stream == stderr;
}

/* Give back the block of the stream, the one the library took. */
static void stream_release_block(FILE *stream)
{
    if (stream->own_block && stream->block != NULL) {
        free(stream->block);
    }

    stream->block = NULL;
    stream->capacity = 0;
    stream->own_block = 0;
    stream->at = 0;
    stream->len = 0;
    stream->used = 0;
}

static void stream_use_single(FILE *stream)
{
    stream->block = &stream->single;
    stream->capacity = 1;
    stream->own_block = 0;
}

/*
 * The mode of a stream that has none yet. A terminal is line buffered and a
 * file or a pipe is block buffered, which is what POSIX asks of the three
 * standard streams and what a program of this system wants of its own: a
 * line of a console arrives when the line is whole, and a filter writes its
 * file in blocks. The error descriptor is never buffered: its lines are the
 * ones a program must not lose when it dies.
 *
 * The block is taken now and not at the end of the file, and a program with
 * no room for it keeps working: the stream becomes unbuffered and the calls
 * reach the descriptor one at a time, which is slower and not wrong.
 */
static int stream_mode(FILE *stream)
{
    if (stream->mode >= 0) {
        return stream->mode;
    }

    stream_list_add(stream);

    if (stream->memory || stream->fd < 0) {
        stream->mode = _IONBF;
        stream_use_single(stream);
        return stream->mode;
    }

    if (stream->fd == ULIBC_STDERR) {
        stream->mode = _IONBF;
    } else if (isatty(stream->fd)) {
        stream->mode = _IOLBF;
    } else {
        stream->mode = _IOFBF;
    }

    if (stream->mode == _IONBF) {
        stream_use_single(stream);
        return stream->mode;
    }

    stream->block = (unsigned char *)malloc(ULIBC_STREAM_BLOCK);
    if (stream->block == NULL) {
        stream->mode = _IONBF;
        stream_use_single(stream);
        return stream->mode;
    }

    stream->capacity = ULIBC_STREAM_BLOCK;
    stream->own_block = 1;

    return stream->mode;
}

/* Write a block of bytes to the descriptor, the whole of it or a short
 * answer when the kernel refused. Returns the bytes written. */
static size_t stream_write_descriptor(FILE *stream, const unsigned char *data, size_t size)
{
    size_t done = 0;

    while (done < size) {
        long wrote = write(stream->fd, data + done, size - done);

        if (wrote < 0) {
            if (errno == EINTR) {
                continue;
            }
            stream->error = 1;
            break;
        }
        if (wrote == 0) {
            stream->error = 1;
            break;
        }
        done += (size_t)wrote;
    }

    return done;
}

/* Give the pending bytes of the block to the descriptor. */
static int stream_flush_output(FILE *stream)
{
    size_t pending = stream->used;
    size_t wrote;

    if (pending == 0) {
        return 0;
    }

    wrote = stream_write_descriptor(stream, stream->block, pending);
    stream->used = 0;

    return (wrote == pending) ? 0 : -1;
}

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
    stream->block = NULL;
    stream->capacity = 0;
    stream->single = 0;
    stream->at = 0;
    stream->len = 0;
    stream->used = 0;
    stream->mode = -1;
    stream->own_block = 0;
    stream->listed = 0;
    stream->next = NULL;

    stream_list_add(stream);

    return stream;
}

/*
 * Fill the block of the stream from its descriptor. A pipe of this system
 * blocks while its writer is there (ADR 0030), so EAGAIN is the answer of a
 * machine that was asked not to wait and the call waits it out for the one
 * that was: it is not the end of the stream. The answer is 1 when bytes
 * arrived, 0 at the end of the file and -1 on an error.
 */
static int stream_fill(FILE *stream)
{
    long rc;

    stream->at = 0;
    stream->len = 0;
    if (stream->eof || stream->capacity == 0) {
        return 0;
    }

    do {
        rc = read(stream->fd, stream->block, stream->capacity);
    } while (rc < 0 && errno == EAGAIN);

    if (rc < 0) {
        stream->error = 1;
        return -1;
    }
    if (rc == 0) {
        stream->eof = 1;
        return 0;
    }

    stream->len = (size_t)rc;
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

    /*
     * The console belongs to the task, not to the stream: a program that
     * closes it gives up the descriptor and not the console of the system.
     * The bytes it wrote and did not flush still leave before the answer.
     */
    if (stream_is_standard(stream)) {
        (void)fflush(stream);
        return 0;
    }

    if (fflush(stream) != 0) {
        rc = EOF;
    }

    if (!stream->memory) {
        if (close(stream->fd) < 0) {
            rc = EOF;
        }
    }

    stream_list_remove(stream);
    stream_release_block(stream);
    free(stream);

    return rc;
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
        bytes[got] = (unsigned char)stream->pushback;
        stream->pushback = -1;
        got++;
    }

    if (stream->memory) {
        size_t left = stream->size - stream->position;
        size_t take = (total - got < left) ? total - got : left;

        memcpy(bytes + got, stream->data + stream->position, take);
        stream->position += take;
        got += take;
        if (got < total) {
            stream->eof = 1;
        }

        return got / size;
    }

    (void)stream_mode(stream);

    while (got < total) {
        size_t left = total - got;

        if (stream->at < stream->len) {
            size_t take = stream->len - stream->at;

            if (take > left) {
                take = left;
            }
            memcpy(bytes + got, stream->block + stream->at, take);
            stream->at += take;
            got += take;
            continue;
        }

        /*
         * A request of a block or more is read into the caller's memory and
         * not through the block: a copy of a whole file is one call of the
         * system after another, and no byte of it needs a second home. The
         * unbuffered stream takes the same path, one byte at a time because
         * that is the request of `fgetc` and not the choice of the library.
         */
        if (left >= stream->capacity) {
            size_t want = (left > STREAM_IO_MAX) ? (size_t)STREAM_IO_MAX : left;
            long rc;

            do {
                rc = read(stream->fd, bytes + got, want);
            } while (rc < 0 && errno == EAGAIN);

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

size_t __ulibc_stream_write(FILE *stream, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;

    if (stream == NULL || data == NULL || size == 0) {
        return 0;
    }

    if (stream->memory) {
        size_t left = stream->size - stream->position;
        size_t take = (size < left) ? size : left;

        if (take > 0) {
            memcpy(stream->data + stream->position, bytes, take);
            stream->position += take;
        }
        if (take < size) {
            stream->error = 1;
            errno = ENOSPC;
        }

        return take;
    }

    if (stream_mode(stream) == _IONBF || stream->capacity == 0) {
        return stream_write_descriptor(stream, bytes, size);
    }

    /*
     * A block that does not fit in the buffer is given to the kernel whole:
     * the buffer of the stream is for the lines and the small writes, and a
     * copy of a megabyte is not helped by a second copy of it.
     */
    if (size >= stream->capacity) {
        if (stream_flush_output(stream) != 0) {
            return 0;
        }

        return stream_write_descriptor(stream, bytes, size);
    }

    while (done < size) {
        size_t room = stream->capacity - stream->used;
        size_t take = (size - done < room) ? size - done : room;

        memcpy(stream->block + stream->used, bytes + done, take);
        stream->used += take;
        done += take;

        if (stream->used == stream->capacity && stream_flush_output(stream) != 0) {
            break;
        }
    }

    /* A line-buffered stream gives the line to the kernel when it is whole,
     * and keeps a last line without its newline until the flush. */
    if (stream->mode == _IOLBF && memchr(bytes, '\n', size) != NULL) {
        (void)stream_flush_output(stream);
    }

    return done;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
    size_t total;

    if (stream == NULL || buffer == NULL || size == 0 || count == 0) {
        return 0;
    }

    total = size * count;
    if (total / size != count) {
        errno = EINVAL;
        stream->error = 1;
        return 0;
    }

    return __ulibc_stream_write(stream, buffer, total) / size;
}

/*
 * The console of a program that links the streams of `src/file.c`: `printf`
 * reaches the block of `stdout` and not the descriptor, so a program that
 * formats a line of a file writes the block and not the line.
 */
int __ulibc_stream_write_stdout(const void *data, size_t size)
{
    if (data == NULL || size == 0) {
        return 0;
    }

    return (__ulibc_stream_write(stdout, data, size) == size) ? 0 : -1;
}

void __ulibc_stream_flush_all(void)
{
    FILE *stream;

    for (stream = g_streams; stream != NULL; stream = stream->next) {
        (void)stream_flush_output(stream);
    }
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
 * The one byte of look-ahead. A byte put back is kept in the state of the
 * stream and not in its block, because the block may be given to the kernel
 * by a seek or a flush; a second call before a read is the error of the
 * standard, and a seek forgets it.
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

    if (__ulibc_stream_write(stream, &value, 1) != 1) {
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
    if (__ulibc_stream_write(stream, text, length) != length) {
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

/*
 * Give the bytes of the stream to the kernel: the block of a stream that
 * writes, and the descriptor position of a stream that reads (the bytes of
 * the read-ahead go back, so that the two positions are one again). The
 * call with no stream is the flush of every stream of the program, which is
 * what the standard asks of `fflush(NULL)`.
 */
int fflush(FILE *stream)
{
    if (stream == NULL) {
        FILE *at;
        int rc = 0;

        for (at = g_streams; at != NULL; at = at->next) {
            if (stream_flush_output(at) != 0) {
                rc = EOF;
            }
        }

        return rc;
    }

    if (stream->memory) {
        return 0; /* the block of the caller is the file */
    }

    (void)stream_mode(stream);

    if (stream_flush_output(stream) != 0) {
        return EOF;
    }

    /*
     * The bytes of the read-ahead are ahead of the descriptor: a stream
     * flushed and read again must start where its reader is. The seek back
     * is possible on a file and not on a pipe, and a pipe has no position
     * to give back: its bytes stay in the block, where the next read finds
     * them.
     */
    if (stream->at < stream->len || stream->pushback >= 0) {
        long back = (long)(stream->len - stream->at);

        if (stream->pushback >= 0) {
            back += 1;
        }

        if (back > 0 && lseek(stream->fd, -back, SEEK_CUR) >= 0) {
            stream->at = 0;
            stream->len = 0;
            stream->pushback = -1;
        }
    }

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

    /*
     * A seek gives the pending bytes to the kernel and takes the read-ahead
     * back, so the descriptor is at the position of the stream and the call
     * of the kernel is the seek itself. A relative seek then counts from
     * the position the reader has reached, not from the block the descriptor
     * is ahead by: a `fseek(f, 1, SEEK_CUR)` after a short read lands one
     * byte after the read and not one byte after the block.
     */
    if (fflush(stream) != 0) {
        stream->error = 1;
        return -1;
    }

    rc = lseek(stream->fd, offset, whence);
    if (rc < 0) {
        stream->error = 1;
        return -1;
    }

    stream->at = 0;
    stream->len = 0;
    stream->used = 0;
    stream->pushback = -1;
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
     * stream is the one of the descriptor less what is still in the block,
     * and less the byte of `ungetc`, which the reader will see again. The
     * bytes written and not given to the kernel are behind it. */
    rc -= (long)(stream->len - stream->at);
    if (stream->pushback >= 0) {
        rc -= 1;
    }
    rc += (long)stream->used;

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

    (void)fflush(stream);

    if (!stream_is_standard(stream)) {
        (void)close(stream->fd);
    }
    stream->fd = opened->fd;
    stream->eof = 0;
    stream->error = 0;
    stream->pushback = -1;
    stream->at = 0;
    stream->len = 0;
    stream->used = 0;
    /* The new file is a descriptor even when the old stream was a block of
     * the caller's memory (`fmemopen` then `freopen`). */
    stream->memory = 0;
    stream->data = NULL;
    stream->size = 0;
    stream->position = 0;
    stream_list_remove(opened);
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
 * The buffering of a stream.
 *
 * `_IONBF` gives the bytes of the stream to the kernel one call at a time;
 * `_IOLBF` keeps them until a newline and gives the line; `_IOFBF` keeps
 * them until the block is full. A `buffer` of the caller is used as it is
 * and is never freed by `fclose`; a NULL buffer takes `size` bytes (or the
 * default block) from the arena of the program.
 *
 * The standard asks for a call before the first I/O of the stream. A call
 * after it is answered too: the bytes of the old block are flushed and the
 * read-ahead goes back to the descriptor, so the new block starts where the
 * reader is. The one case this cannot undo is a pipe, which has no position
 * to give the read-ahead back to; a stream that is read must be set before
 * its first read, as in every library.
 */
int setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    if (stream == NULL || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) {
        errno = EINVAL;
        return -1;
    }

    if (stream->memory) {
        stream->mode = _IONBF;
        return 0;
    }

    if (fflush(stream) != 0) {
        return -1;
    }

    stream_release_block(stream);

    if (mode == _IONBF) {
        stream->mode = _IONBF;
        stream_use_single(stream);
        return 0;
    }

    if (size == 0 && buffer == NULL) {
        size = ULIBC_STREAM_BLOCK;
    }
    if (size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (buffer != NULL) {
        stream->block = (unsigned char *)buffer;
        stream->own_block = 0;
    } else {
        stream->block = (unsigned char *)malloc(size);
        if (stream->block == NULL) {
            errno = ENOMEM;
            stream->mode = -1;
            return -1;
        }
        stream->own_block = 1;
    }

    stream->capacity = size;
    stream->mode = mode;

    return 0;
}
