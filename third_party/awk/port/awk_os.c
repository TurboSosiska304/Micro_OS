/*
 * Micro OS — the calls the One True Awk makes and the system does not answer.
 *
 * Everything here is the port layer of `third_party/awk`, not a change of the
 * release: the names are declared by `port/awk_port.h`, which is force-included
 * into the sources of the library. The file is compiled for the guest alone
 * (`microos_userland_library`); the host build of the test links the C library
 * of the host for the same symbols.
 *
 * The two calls of a Unix-like system are the shape of a machine without a
 * shell: `system` and `popen` say so on the error descriptor and answer the
 * failure, and a script that asks for them stops with a sentence a person can
 * read instead of a success that did nothing. The row of the debt is in
 * `docs/debts.md`.
 *
 * The rest is small and mechanical: the copy of a word, the compare without
 * the case, the generator of `rand`/`srand` of POSIX, the flag of `fcntl`
 * that has no meaning here, and the identity of a byte and a wide character
 * in the C locale of the system. None of it is a second implementation of
 * something the library of a program already has: `components/libc/user` has
 * no `strdup`, no `strncasecmp`, no `random` and no `popen` today, and the
 * note of the port says why they live with the source that asks for them.
 */
#include "ulibc.h"

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <wctype.h>

#include "awk_port.h"

/* The binary search of the C library over the table of the callers. The
 * order is the one of the function; the answer is NULL when the key is not
 * there, as the standard says. */
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    size_t low = 0;
    size_t high = count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const void *item = (const char *)base + middle * size;
        int order = compare(key, item);

        if (order < 0) {
            high = middle;
        } else if (order > 0) {
            low = middle + 1;
        } else {
            return (void *)item;
        }
    }

    return NULL;
}

int putc(int byte, FILE *stream)
{
    return fputc(byte, stream);
}

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

int strncasecmp(const char *left, const char *right, size_t count)
{
    while (count > 0) {
        int first = tolower((unsigned char)*left);
        int second = tolower((unsigned char)*right);

        if (first != second) {
            return first - second;
        }
        if (first == '\0') {
            break;
        }
        left++;
        right++;
        count--;
    }

    return 0;
}

/*
 * The additive generator of POSIX: a table of 31 words and the sum of the word
 * three places back and the last one, which has a period of about 2^69 and is
 * the shape of the `random` of a Unix-like system. The sequence is not the
 * table of glibc - POSIX does not name one - but the same seed gives the same
 * numbers here on every run, which is what `srand(1)` of a script expects.
 */
#define AWK_RANDOM_DEGREE 31
#define AWK_RANDOM_SEPARATION 3
#define AWK_RANDOM_MASK 0x7fffffffL

static uint32_t g_random_state_s[AWK_RANDOM_DEGREE];
static size_t g_random_front_s;
static size_t g_random_rear_s;

void srandom(unsigned int seed)
{
    size_t index;

    g_random_state_s[0] = seed;
    for (index = 1; index < AWK_RANDOM_DEGREE; index++) {
        g_random_state_s[index] =
            1103515245u * g_random_state_s[index - 1] + 12345u;
    }
    g_random_front_s = AWK_RANDOM_SEPARATION;
    g_random_rear_s = 0;
}

long random(void)
{
    uint32_t value = g_random_state_s[g_random_front_s] + g_random_state_s[g_random_rear_s];

    g_random_state_s[g_random_front_s] = value;
    if (++g_random_front_s == AWK_RANDOM_DEGREE) {
        g_random_front_s = 0;
    }
    if (++g_random_rear_s == AWK_RANDOM_DEGREE) {
        g_random_rear_s = 0;
    }

    return (long)(value & (uint32_t)AWK_RANDOM_MASK);
}

int fcntl(int descriptor, int command, ...)
{
    (void)descriptor;
    (void)command;

    /* Close-on-exec is a property of the descriptor table of this system that
     * does not exist: a program of the machine has four descriptors and no
     * second program of its own to give them to. The call is accepted so that
     * a source that asks for the flag reads as the release wrote it. */
    return 0;
}

int system(const char *command)
{
    (void)command;

    fprintf(stderr, "awk: system: this machine has no shell\n");
    errno = ENOSYS;
    return -1;
}

FILE *popen(const char *command, const char *mode)
{
    (void)command;
    (void)mode;

    fprintf(stderr, "awk: popen: this machine has no shell and no second process\n");
    errno = ENOSYS;
    return NULL;
}

int pclose(FILE *stream)
{
    (void)stream;

    errno = ENOSYS;
    return -1;
}

/*
 * A byte is a character: the C locale of the system. The UTF-8 paths of the
 * release are compiled (`MB_CUR_MAX` is 1, so they are not taken) and these
 * two calls are the identity a caller of the standard expects even so.
 */
int mbtowc(wchar_t *wide, const char *text, size_t count)
{
    if (text == NULL) {
        return 0; /* the call that resets the state */
    }
    if (count == 0) {
        return -1;
    }
    if (text[0] == '\0') {
        return 0;
    }
    if (wide != NULL) {
        *wide = (unsigned char)text[0];
    }

    return 1;
}

int wctomb(char *text, wchar_t wide)
{
    if (text == NULL) {
        return 0; /* the call that resets the state */
    }

    text[0] = (char)wide;
    return 1;
}

wint_t towupper(wint_t wide)
{
    if (wide < 256) {
        return (wint_t)toupper((int)wide);
    }
    return wide;
}

wint_t towlower(wint_t wide)
{
    if (wide < 256) {
        return (wint_t)tolower((int)wide);
    }
    return wide;
}
