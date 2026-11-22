/*
 * Micro OS — a copy of a text in the arena of a program.
 *
 * The function is the one of the C library and it is a file of its own
 * because it names the allocator: kept in `src/string.c`, the reference would
 * be a symbol of the object of every program, and an image that never copies
 * a text would still have to link the arena. A program that calls `strdup`
 * names this file in its sources (bc is the first).
 */
#include "ulibc.h"

#include <errno.h>

char *strdup(const char *text)
{
    size_t length;
    char *copy;

    if (text == NULL) {
        errno = EINVAL;
        return NULL;
    }

    length = strlen(text) + 1;
    copy = (char *)malloc(length);
    if (copy == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(copy, text, length);

    return copy;
}
