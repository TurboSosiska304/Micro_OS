/*
 * Micro OS — the strings a program formats and prints.
 *
 * A program of this system has no libc of its own under it: the image carries
 * what it needs. These are the four functions a formatter and a line of
 * arguments need, written without a call of the string library of a host that
 * is not there.
 */
#include "ulibc.h"

size_t strlen(const char *text)
{
    size_t length = 0;

    while (text[length] != '\0') {
        length++;
    }

    return length;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        left++;
        right++;
    }

    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        unsigned char a = (unsigned char)left[i];
        unsigned char b = (unsigned char)right[i];

        if (a != b) {
            return (int)a - (int)b;
        }
        if (a == '\0') {
            return 0;
        }
    }

    return 0;
}

void *memcpy(void *dest, const void *source, size_t count)
{
    unsigned char *to = (unsigned char *)dest;
    const unsigned char *from = (const unsigned char *)source;
    size_t i;

    for (i = 0; i < count; i++) {
        to[i] = from[i];
    }

    return dest;
}

void *memset(void *dest, int byte, size_t count)
{
    unsigned char *to = (unsigned char *)dest;
    size_t i;

    for (i = 0; i < count; i++) {
        to[i] = (unsigned char)byte;
    }

    return dest;
}
