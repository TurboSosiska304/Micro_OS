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

char *strcpy(char *dest, const char *source)
{
    char *start = dest;

    while ((*dest++ = *source++) != '\0') {
    }

    return start;
}

char *strncpy(char *dest, const char *source, size_t count)
{
    size_t index;

    for (index = 0; index < count && source[index] != '\0'; index++) {
        dest[index] = source[index];
    }

    for (; index < count; index++) {
        dest[index] = '\0';
    }

    return dest;
}

char *strcat(char *dest, const char *source)
{
    (void)strcpy(dest + strlen(dest), source);
    return dest;
}

char *strchr(const char *text, int byte)
{
    char wanted = (char)byte;

    for (;;) {
        if (*text == wanted) {
            return (char *)text;
        }
        if (*text == '\0') {
            return NULL;
        }
        text++;
    }
}

char *strrchr(const char *text, int byte)
{
    char wanted = (char)byte;
    const char *found = NULL;

    for (;;) {
        if (*text == wanted) {
            found = text;
        }
        if (*text == '\0') {
            return (char *)found;
        }
        text++;
    }
}

void *memcpy(void *dest, const void *source, size_t count)
{
    unsigned char *to = (unsigned char *)dest;
    const unsigned char *from = (const unsigned char *)source;

    while (count > 0u && (((size_t)to) & 7u) != 0u) {
        *to++ = *from++;
        count--;
    }

    while (count >= 8u) {
        __builtin_memcpy(to, from, 8u);
        to += 8u;
        from += 8u;
        count -= 8u;
    }

    while (count > 0u) {
        *to++ = *from++;
        count--;
    }

    return dest;
}

void *memmove(void *dest, const void *source, size_t count)
{
    unsigned char *to = (unsigned char *)dest;
    const unsigned char *from = (const unsigned char *)source;

    if (to == from || count == 0u) {
        return dest;
    }

    if (to < from || to >= from + count) {
        return memcpy(dest, source, count);
    }

    to += count;
    from += count;

    while (count > 0u && (((size_t)to) & 7u) != 0u) {
        to--;
        from--;
        *to = *from;
        count--;
    }

    while (count >= 8u) {
        to -= 8u;
        from -= 8u;
        count -= 8u;
        __builtin_memcpy(to, from, 8u);
    }

    while (count > 0u) {
        to--;
        from--;
        *to = *from;
        count--;
    }

    return dest;
}

void *memset(void *dest, int byte, size_t count)
{
    unsigned char *to = (unsigned char *)dest;
    unsigned char value = (unsigned char)byte;
    unsigned long long word = (unsigned long long)value;

    word |= word << 8;
    word |= word << 16;
    word |= word << 32;

    while (count > 0u && (((size_t)to) & 7u) != 0u) {
        *to++ = value;
        count--;
    }

    while (count >= 8u) {
        __builtin_memcpy(to, &word, 8u);
        to += 8u;
        count -= 8u;
    }

    while (count > 0u) {
        *to++ = value;
        count--;
    }

    return dest;
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

void *memchr(const void *memory, int byte, size_t count)
{
    const unsigned char *bytes = (const unsigned char *)memory;
    unsigned char wanted = (unsigned char)byte;
    size_t i;

    for (i = 0; i < count; i++) {
        if (bytes[i] == wanted) {
            return (void *)(bytes + i);
        }
    }

    return NULL;
}

/*
 * The comparison of the collation of the C locale: the bytes themselves. The
 * standard puts `strcoll` in the library for a program that sorts strings,
 * and `strcmp` is the answer of the one locale the system has.
 */
int strcoll(const char *left, const char *right)
{
    return strcmp(left, right);
}

size_t strspn(const char *text, const char *accept)
{
    size_t count = 0;

    while (text[count] != '\0' && strchr(accept, text[count]) != NULL) {
        count++;
    }

    return count;
}

size_t strcspn(const char *text, const char *reject)
{
    size_t count = 0;

    while (text[count] != '\0' && strchr(reject, text[count]) == NULL) {
        count++;
    }

    return count;
}

char *strpbrk(const char *text, const char *accept)
{
    while (*text != '\0') {
        if (strchr(accept, *text) != NULL) {
            return (char *)text;
        }
        text++;
    }

    return NULL;
}
