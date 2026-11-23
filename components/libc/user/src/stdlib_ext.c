/*
 * Micro OS — the numbers and the order of a program.
 *
 * The conversions are the ones of POSIX with the end of the conversion and the
 * overflow of `ERANGE`; the sort is a heap sort, which has no recursion and no
 * second buffer, and the search is the halving walk of `bsearch`. There is no
 * real number here: a program of this machine is built without the registers
 * of the floating point unit (`-mgeneral-regs-only`, the port saves no XMM
 * state), so a `double` cannot cross a function of the guest and `strtod` is
 * not in the library. `sort` compares its numbers as decimals in the port
 * layer of sbase; `docs/porting-programs.md` names the rule.
 *
 * The environment of a program is the vector `_start` was entered with; the
 * library keeps the pointer so that `getenv` can read the four words of the
 * system. The handlers of `atexit` run when `exit` ends the program, in the
 * reverse order of their registration.
 */
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

extern char *const *__ulibc_environ;

static int digit_value(int letter)
{
    if (letter >= '0' && letter <= '9') {
        return letter - '0';
    }
    if (letter >= 'a' && letter <= 'z') {
        return letter - 'a' + 10;
    }
    if (letter >= 'A' && letter <= 'Z') {
        return letter - 'A' + 10;
    }

    return 99;
}

/*
 * The common walk of the four conversions: the space before the number, the
 * sign, the base and the digits. The answer is the magnitude and the flag of
 * an overflow; a number that does not fit the unsigned width is clamped and
 * reported with ERANGE by the caller.
 */
static unsigned long long convert(const char *text, char **end, int base, int *negative,
                                  int *overflow)
{
    const char *place = text;
    unsigned long long value = 0;
    int any = 0;

    *negative = 0;
    *overflow = 0;

    while (*place == ' ' || *place == '\t' || *place == '\n' || *place == '\r' || *place == '\v' ||
           *place == '\f') {
        place++;
    }

    if (*place == '+' || *place == '-') {
        *negative = (*place == '-');
        place++;
    }

    if (base == 0) {
        if (place[0] == '0' && (place[1] == 'x' || place[1] == 'X') && digit_value(place[2]) < 16) {
            base = 16;
            place += 2;
        } else if (place[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16 && place[0] == '0' && (place[1] == 'x' || place[1] == 'X') &&
               digit_value(place[2]) < 16) {
        place += 2;
    }

    if (base < 2 || base > 36) {
        if (end != NULL) {
            *end = (char *)text;
        }
        errno = EINVAL;
        return 0;
    }

    for (;;) {
        int digit = digit_value((unsigned char)*place);

        if (digit >= base) {
            break;
        }

        any = 1;
        if (value > (~0ull - (unsigned long long)digit) / (unsigned long long)base) {
            *overflow = 1;
            value = ~0ull;
        } else {
            value = value * (unsigned long long)base + (unsigned long long)digit;
        }
        place++;
    }

    if (end != NULL) {
        *end = (char *)(any ? place : text);
    }
    if (!any) {
        errno = EINVAL;
    }

    return value;
}

long long strtoll(const char *text, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value;
    unsigned long long limit;

    if (text == NULL) {
        if (end != NULL) {
            *end = NULL;
        }
        errno = EINVAL;
        return 0;
    }

    value = convert(text, end, base, &negative, &overflow);
    limit = negative ? (unsigned long long)LLONG_MAX + 1ull : (unsigned long long)LLONG_MAX;

    if (overflow || value > limit) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }

    if (negative) {
        return (value == (unsigned long long)LLONG_MAX + 1ull) ? LLONG_MIN : -(long long)value;
    }

    return (long long)value;
}

unsigned long long strtoull(const char *text, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value;

    if (text == NULL) {
        if (end != NULL) {
            *end = NULL;
        }
        errno = EINVAL;
        return 0;
    }

    value = convert(text, end, base, &negative, &overflow);
    if (overflow) {
        errno = ERANGE;
        return ~0ull;
    }

    return negative ? (unsigned long long)0 - value : value;
}

long strtol(const char *text, char **end, int base)
{
    long long value = strtoll(text, end, base);

    if (value > LONG_MAX) {
        errno = ERANGE;
        return LONG_MAX;
    }
    if (value < LONG_MIN) {
        errno = ERANGE;
        return LONG_MIN;
    }

    return (long)value;
}

unsigned long strtoul(const char *text, char **end, int base)
{
    unsigned long long value = strtoull(text, end, base);

    if (value > ULONG_MAX) {
        errno = ERANGE;
        return ULONG_MAX;
    }

    return (unsigned long)value;
}

int atoi(const char *text)
{
    return (int)strtol(text, NULL, 10);
}

long atol(const char *text)
{
    return strtol(text, NULL, 10);
}

long long atoll(const char *text)
{
    return strtoll(text, NULL, 10);
}

int abs(int value)
{
    return (value < 0) ? -value : value;
}

long labs(long value)
{
    return (value < 0) ? -value : value;
}

long long llabs(long long value)
{
    return (value < 0) ? -value : value;
}

static void swap_bytes(unsigned char *left, unsigned char *right, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        unsigned char byte = left[i];

        left[i] = right[i];
        right[i] = byte;
    }
}

/* The heap of `qsort`: the element at `root` sinks to its place. */
static void heap_sift(unsigned char *base, size_t root, size_t count, size_t size,
                      int (*compare)(const void *, const void *))
{
    for (;;) {
        size_t child = 2 * root + 1;
        size_t best = root;

        if (child < count && compare(base + child * size, base + best * size) > 0) {
            best = child;
        }
        if (child + 1 < count && compare(base + (child + 1) * size, base + best * size) > 0) {
            best = child + 1;
        }
        if (best == root) {
            return;
        }

        swap_bytes(base + root * size, base + best * size, size);
        root = best;
    }
}

void qsort(void *array, size_t count, size_t size, int (*compare)(const void *, const void *))
{
    unsigned char *base = (unsigned char *)array;
    size_t index;

    if (array == NULL || size == 0 || count < 2 || compare == NULL) {
        return;
    }

    for (index = count / 2; index > 0; index--) {
        heap_sift(base, index - 1, count, size, compare);
    }

    for (index = count; index > 1; index--) {
        swap_bytes(base, base + (index - 1) * size, size);
        heap_sift(base, 0, index - 1, size, compare);
    }
}

void *bsearch(const void *key, const void *array, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    const unsigned char *base = (const unsigned char *)array;
    size_t low = 0;
    size_t high = count;

    if (key == NULL || array == NULL || size == 0 || compare == NULL) {
        return NULL;
    }

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        int order = compare(key, base + middle * size);

        if (order < 0) {
            high = middle;
        } else if (order > 0) {
            low = middle + 1;
        } else {
            return (void *)(base + middle * size);
        }
    }

    return NULL;
}

char *getenv(const char *name)
{
    size_t length;
    int i;

    if (name == NULL || __ulibc_environ == NULL) {
        return NULL;
    }

    length = strlen(name);
    for (i = 0; __ulibc_environ[i] != NULL; i++) {
        if (strncmp(__ulibc_environ[i], name, length) == 0 && __ulibc_environ[i][length] == '=') {
            return (char *)__ulibc_environ[i] + length + 1;
        }
    }

    return NULL;
}

void abort(void)
{
    static const char sentence[] = "the program called abort()\n";

    (void)write(ULIBC_STDERR, sentence, sizeof(sentence) - 1);
    _exit(255);

    for (;;) {
    }
}
