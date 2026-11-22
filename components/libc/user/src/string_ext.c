/*
 * Micro OS — the strings a portable source uses and the library did not have.
 *
 * The core of the strings is `src/string.c`; this file adds the functions a
 * command of a Unix-like system leans on and the kernel has no reason to
 * carry: the copy of a word in the arena, the span of a set, and the compare
 * without the case of a letter. The errors of the failures are the ones of
 * POSIX, so a caller reads `errno` and not the sign of the answer.
 */
#include "ulibc.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

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

char *strndup(const char *text, size_t count)
{
    size_t length = 0;
    char *copy;

    if (text == NULL) {
        errno = EINVAL;
        return NULL;
    }

    while (length < count && text[length] != '\0') {
        length++;
    }

    copy = (char *)malloc(length + 1);
    if (copy == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    memcpy(copy, text, length);
    copy[length] = '\0';

    return copy;
}

size_t strspn(const char *text, const char *set)
{
    size_t count = 0;

    while (text[count] != '\0' && strchr(set, text[count]) != NULL) {
        count++;
    }

    return count;
}

size_t strcspn(const char *text, const char *set)
{
    size_t count = 0;

    while (text[count] != '\0' && strchr(set, text[count]) == NULL) {
        count++;
    }

    return count;
}

char *strpbrk(const char *text, const char *set)
{
    while (*text != '\0') {
        if (strchr(set, *text) != NULL) {
            return (char *)text;
        }
        text++;
    }

    return NULL;
}

int strcasecmp(const char *left, const char *right)
{
    while (*left != '\0') {
        int a = tolower((unsigned char)*left);
        int b = tolower((unsigned char)*right);

        if (a != b) {
            return a - b;
        }
        left++;
        right++;
    }

    return tolower((unsigned char)*left) - tolower((unsigned char)*right);
}

int strncasecmp(const char *left, const char *right, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        int a = tolower((unsigned char)left[i]);
        int b = tolower((unsigned char)right[i]);

        if (a != b) {
            return a - b;
        }
        if (a == 0) {
            return 0;
        }
    }

    return 0;
}

char *strcasestr(const char *text, const char *needle)
{
    size_t length;

    if (text == NULL || needle == NULL) {
        return NULL;
    }

    length = strlen(needle);
    if (length == 0) {
        return (char *)text;
    }

    while (*text != '\0') {
        if (strncasecmp(text, needle, length) == 0) {
            return (char *)text;
        }
        text++;
    }

    return NULL;
}
