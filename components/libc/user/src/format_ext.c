/*
 * Micro OS — the formatter of a stream and the reader of a line.
 *
 * `printf` writes to descriptor 1 through the formatter of `src/stdio.c`; a
 * source that prints to `stderr` or to a file calls `fprintf`, and the format
 * is the same one. The text is formatted into the arena of the program (its
 * length is known after the first walk over the arguments, as `snprintf` of
 * C99 promises) and written to the stream in one call; a line of any length
 * leaves in one piece.
 *
 * `getline` is the line reader a command of a Unix-like system is built on: it
 * grows the buffer of the caller, keeps the delimiter, terminates the text and
 * answers its length. The stream of a program has no read-ahead buffer, so the
 * reader reaches the kernel for every byte: a file of this system is small and
 * a line is short, and a buffer of the library would be a second policy for
 * the end of a file and the error of a call.
 */
#include <stdio.h>

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

int sprintf(char *buffer, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buffer, (size_t)-1, format, args);
    va_end(args);

    return length;
}

int vfprintf(FILE *stream, const char *format, va_list args)
{
    va_list measure;
    char *text;
    int length;
    size_t wrote;

    if (stream == NULL || format == NULL) {
        errno = EINVAL;
        return -1;
    }

    va_copy(measure, args);
    length = vsnprintf(NULL, 0, format, measure);
    va_end(measure);

    if (length < 0) {
        return -1;
    }
    if (length == 0) {
        return 0;
    }

    text = (char *)malloc((size_t)length + 1);
    if (text == NULL) {
        errno = ENOMEM;
        return -1;
    }

    (void)vsnprintf(text, (size_t)length + 1, format, args);
    wrote = fwrite(text, 1, (size_t)length, stream);
    free(text);

    if (wrote != (size_t)length) {
        return -1;
    }

    return length;
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

int vprintf(const char *format, va_list args)
{
    return vfprintf(stdout, format, args);
}

int putchar(int byte)
{
    return fputc(byte, stdout);
}

int getchar(void)
{
    return fgetc(stdin);
}

/*
 * The sentence of the last error. The number is the one of `errno`, the text
 * is the one of `strerror`, and the prefix of the caller goes in front of it
 * with a colon, as the library of a Unix-like system prints it.
 */
void perror(const char *text)
{
    int code = errno;

    if (text != NULL && text[0] != '\0') {
        (void)fprintf(stderr, "%s: %s\n", text, strerror(code));
    } else {
        (void)fprintf(stderr, "%s\n", strerror(code));
    }
}

/*
 * The next line of a stream, with its delimiter. The buffer belongs to the
 * caller and grows as the line does; a line that does not fit is a line of the
 * next call of the allocator. The answer is the number of bytes with the
 * delimiter, or -1 at the end of the file with nothing read.
 */
ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *stream)
{
    size_t used = 0;
    char *buffer;

    if (line == NULL || capacity == NULL || stream == NULL) {
        errno = EINVAL;
        return -1;
    }

    buffer = *line;
    if (buffer == NULL || *capacity == 0) {
        *capacity = 64;
        buffer = (char *)malloc(*capacity);
        if (buffer == NULL) {
            errno = ENOMEM;
            return -1;
        }
        *line = buffer;
    }

    for (;;) {
        int byte = fgetc(stream);

        if (byte == EOF) {
            if (used == 0) {
                return -1;
            }
            break;
        }

        if (used + 1 >= *capacity) {
            size_t grown = *capacity * 2;
            char *bigger = (char *)realloc(buffer, grown);

            if (bigger == NULL) {
                errno = ENOMEM;
                return -1;
            }
            buffer = bigger;
            *capacity = grown;
            *line = buffer;
        }

        buffer[used++] = (char)byte;
        if (byte == delimiter) {
            break;
        }
    }

    buffer[used] = '\0';

    return (ssize_t)used;
}

ssize_t getline(char **line, size_t *capacity, FILE *stream)
{
    return getdelim(line, capacity, '\n', stream);
}
