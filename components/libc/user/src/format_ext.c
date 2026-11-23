/*
 * Micro OS — the reader of a line of a stream.
 *
 * The formatter of a program is `src/stdio.c` and the streams are
 * `src/file.c`: one `printf` and one `fprintf` for the whole tree, whichever
 * program links this file. What is here is the line reader a command of a
 * Unix-like system is built on: it grows the buffer of the caller, keeps the
 * delimiter, terminates the text and answers its length. The stream of a
 * program keeps a block of a few hundred bytes (src/file.c), so the reader
 * asks the stream for a byte and the stream reaches the kernel once per block.
 */
#include <stdio.h>

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

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
