/*
 * Micro OS — the few bytes of the C library the loader needs.
 *
 * The compiler is free to turn a copy of a structure or a clear of a buffer
 * into a call to memcpy() or memset(), and there is no library under the
 * loader to answer it. These definitions are that library; the names are the
 * ones GCC expects, and nothing here allocates or touches a device.
 */

#include <stddef.h>

void *memcpy(void *destination, const void *source, size_t count)
{
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;
    size_t i;

    for (i = 0; i < count; i++) {
        to[i] = from[i];
    }

    return destination;
}

void *memset(void *destination, int value, size_t count)
{
    unsigned char *to = (unsigned char *)destination;
    size_t i;

    for (i = 0; i < count; i++) {
        to[i] = (unsigned char)value;
    }

    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    unsigned char *to = (unsigned char *)destination;
    const unsigned char *from = (const unsigned char *)source;
    size_t i;

    if (to < from) {
        for (i = 0; i < count; i++) {
            to[i] = from[i];
        }
    } else {
        for (i = count; i > 0; i--) {
            to[i - 1] = from[i - 1];
        }
    }

    return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = (const unsigned char *)left;
    const unsigned char *b = (const unsigned char *)right;
    size_t i;

    for (i = 0; i < count; i++) {
        if (a[i] != b[i]) {
            return (int)a[i] - (int)b[i];
        }
    }

    return 0;
}
