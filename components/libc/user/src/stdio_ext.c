/*
 * Micro OS — the line reader and the sentence of the last error.
 *
 * `perror` and `getline` are the two calls a command of a Unix-like system is
 * built on and the formatter of `src/stdio.c` does not carry: a program that
 * prints a line of a file links this file, and a program that only formats a
 * sentence does not pay for a reader that grows a buffer. The stream of a
 * program has no read-ahead, so the reader reaches the kernel for every byte;
 * a file of this system is small and a line is short, and a buffer of the
 * library would be a second policy for the end of a file and the error of a
 * call.
 */
#include <stdio.h>

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

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
