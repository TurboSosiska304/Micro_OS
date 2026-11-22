/*
 * Micro OS — the strings a program formats and prints.
 *
 * A program of this system has no libc of its own under it: the image carries
 * what it needs. These are the functions a formatter, a line of arguments and
 * a search of a line need, written without a call of the string library of a
 * host that is not there.
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

char *strstr(const char *text, const char *needle)
{
    size_t length = strlen(needle);

    if (length == 0) {
        return (char *)text;
    }

    while (*text != '\0') {
        if (strncmp(text, needle, length) == 0) {
            return (char *)text;
        }
        text++;
    }

    return NULL;
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

void *memchr(const void *text, int letter, size_t count)
{
    const unsigned char *bytes = (const unsigned char *)text;
    size_t i;

    for (i = 0; i < count; i++) {
        if (bytes[i] == (unsigned char)letter) {
            return (void *)(bytes + i);
        }
    }

    return NULL;
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

void *memmove(void *dest, const void *source, size_t count)
{
    unsigned char *to = (unsigned char *)dest;
    const unsigned char *from = (const unsigned char *)source;
    size_t i;

    if (to == from || count == 0) {
        return dest;
    }

    /* A copy that overlaps forward would read a byte it has already
     * written, so that direction goes from the end. */
    if ((size_t)to < (size_t)from) {
        for (i = 0; i < count; i++) {
            to[i] = from[i];
        }
    } else {
        for (i = count; i > 0; i--) {
            to[i - 1] = from[i - 1];
        }
    }

    return dest;
}

char *strchr(const char *text, int letter)
{
    unsigned char wanted = (unsigned char)letter;

    while (*text != '\0') {
        if ((unsigned char)*text == wanted) {
            return (char *)text;
        }
        text++;
    }

    return wanted == 0 ? (char *)text : NULL;
}

char *strrchr(const char *text, int letter)
{
    unsigned char wanted = (unsigned char)letter;
    const char *found = NULL;

    for (;;) {
        if ((unsigned char)*text == wanted) {
            found = text;
        }
        if (*text == '\0') {
            return (char *)found;
        }
        text++;
    }
}
