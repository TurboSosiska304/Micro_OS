/*
 * Micro OS — the conversions and the sort of a program.
 *
 * The functions are the ones a portable source expects of `<stdlib.h>`, with
 * the shape of the C library: the conversion answers the value and leaves the
 * end of the text in `*end`, the error of an overflow is `errno = ERANGE` and
 * a sort answers its order through the comparison of the caller.
 *
 * The conversion of a float is written here and not asked of a library of the
 * host: the image of a program carries every function it calls. It takes the
 * decimal form of the C locale - an optional sign, digits, a point and an
 * optional exponent - and not the hexadecimal form of C99, which no source of
 * this tree writes. The value is accumulated as a whole number of up to 19
 * digits and scaled by the exact powers of ten a double can hold; the last
 * bit of a long text is therefore the rounding of the machine, not of a
 * decimal library, and that is the debt of this file.
 */
#include "ulibc.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

static int space_byte(int byte)
{
    return byte == ' ' || (byte >= '\t' && byte <= '\r');
}

static int digit_of(int byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'z') {
        return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'Z') {
        return byte - 'A' + 10;
    }

    return -1;
}

#ifdef ULIBC_WITH_FLOAT
/* The powers of ten that a double holds exactly, for the scaling below. */
static const double g_pow10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static int word_is(const char *text, const char *word)
{
    while (*word != '\0') {
        char left = *text;
        char right = *word;

        if (left >= 'A' && left <= 'Z') {
            left = (char)(left - 'A' + 'a');
        }
        if (left != right) {
            return 0;
        }
        text++;
        word++;
    }

    return 1;
}
#endif /* ULIBC_WITH_FLOAT */

#ifdef ULIBC_WITH_FLOAT
/*
 * The text of a real number. It is compiled only into a program built with
 * `ULIBC_WITH_FLOAT` and without `-mgeneral-regs-only`: a function that
 * returns a `double` needs an XMM register, and a program of the tree does
 * not carry one.
 */
double strtod(const char *text, char **end)
{
    const char *cursor = text;
    unsigned long long mantissa = 0;
    double value;
    int digits = 0;
    int exponent = 0;
    int negative = 0;
    int any = 0;

    while (space_byte((unsigned char)*cursor)) {
        cursor++;
    }

    if (*cursor == '+' || *cursor == '-') {
        negative = (*cursor == '-');
        cursor++;
    }

    /* The values that are not numerals of the decimal form: the infinity and
     * the not-a-number. Lua's `tonumber("inf")` is the call that asks. */
    if (word_is(cursor, "inf")) {
        cursor += 3;
        if (word_is(cursor, "inity")) {
            cursor += 5;
        }
        if (end != NULL) {
            *end = (char *)cursor;
        }
        return negative ? -HUGE_VAL : HUGE_VAL;
    }
    if (word_is(cursor, "nan")) {
        cursor += 3;
        if (end != NULL) {
            *end = (char *)cursor;
        }
        return NAN;
    }

    while (*cursor >= '0' && *cursor <= '9') {
        if (digits < 19) {
            mantissa = mantissa * 10ull + (unsigned long long)(*cursor - '0');
            digits++;
        } else {
            exponent++;
        }
        any = 1;
        cursor++;
    }

    if (*cursor == '.') {
        cursor++;
        while (*cursor >= '0' && *cursor <= '9') {
            if (digits < 19) {
                mantissa = mantissa * 10ull + (unsigned long long)(*cursor - '0');
                digits++;
                exponent--;
            }
            any = 1;
            cursor++;
        }
    }

    if (!any) {
        if (end != NULL) {
            *end = (char *)text;
        }
        return 0.0;
    }

    if (*cursor == 'e' || *cursor == 'E') {
        const char *mark = cursor;
        int power = 0;
        int power_negative = 0;

        cursor++;
        if (*cursor == '+' || *cursor == '-') {
            power_negative = (*cursor == '-');
            cursor++;
        }
        if (*cursor >= '0' && *cursor <= '9') {
            while (*cursor >= '0' && *cursor <= '9') {
                if (power < 100000) {
                    power = power * 10 + (*cursor - '0');
                }
                cursor++;
            }
            exponent += power_negative ? -power : power;
        } else {
            cursor = mark; /* an `e` with no digits after it is not one */
        }
    }

    value = (double)mantissa;

    /* The scale of the decimal point and the exponent. The steps are the
     * exact powers a double has, so a number of one or two of them is the
     * value the decimal text names. */
    while (exponent > 0) {
        int step = (exponent > 22) ? 22 : exponent;

        value *= g_pow10[step];
        exponent -= step;
        if (value > HUGE_VAL / 2.0) {
            errno = ERANGE;
            value = HUGE_VAL;
            break;
        }
    }
    while (exponent < 0) {
        int step = (exponent < -22) ? -22 : exponent;

        value /= g_pow10[-step];
        exponent -= step;
        if (value < 1e-308 && value != 0.0) {
            errno = ERANGE;
            break;
        }
    }

    if (end != NULL) {
        *end = (char *)cursor;
    }

    return negative ? -value : value;
}

double atof(const char *text)
{
    return strtod(text, NULL);
}
#endif /* ULIBC_WITH_FLOAT */

/*
 * The whole numbers. The sign is read from the text and the digits are
 * accumulated in the unsigned width of the result, which is what makes the
 * most negative number expressible: a `long long` has no positive counterpart
 * of the same width, and the C library takes its magnitude this way.
 */
static unsigned long long convert_whole(const char *text, char **end, int base, int *negative,
                                        int *overflow)
{
    const char *cursor = text;
    unsigned long long value = 0;
    int any = 0;

    *negative = 0;
    *overflow = 0;

    while (space_byte((unsigned char)*cursor)) {
        cursor++;
    }

    if (*cursor == '+' || *cursor == '-') {
        *negative = (*cursor == '-');
        cursor++;
    }

    if ((base == 0 || base == 16) && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
        base = 16;
        cursor += 2;
    } else if (base == 0) {
        base = (cursor[0] == '0') ? 8 : 10;
    }

    if (base < 2 || base > 36) {
        if (end != NULL) {
            *end = (char *)text;
        }
        return 0;
    }

    while (*cursor != '\0') {
        int digit = digit_of((unsigned char)*cursor);

        if (digit < 0 || digit >= base) {
            break;
        }
        any = 1;

        if (value > (ULLONG_MAX - (unsigned long long)digit) / (unsigned long long)base) {
            *overflow = 1;
        } else if (!*overflow) {
            value = value * (unsigned long long)base + (unsigned long long)digit;
        }
        cursor++;
    }

    if (!any) {
        if (end != NULL) {
            *end = (char *)text;
        }
        return 0;
    }

    if (end != NULL) {
        *end = (char *)cursor;
    }

    return value;
}

unsigned long long strtoull(const char *text, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value = convert_whole(text, end, base, &negative, &overflow);

    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }

    /* The standard negates the value of a text with a minus sign in the
     * unsigned width, without an error: the result is the one of the two's
     * complement of the number. */
    return negative ? (unsigned long long)0 - value : value;
}

long long strtoll(const char *text, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value = convert_whole(text, end, base, &negative, &overflow);
    unsigned long long magnitude = negative ? (unsigned long long)0 - value : value;

    if (overflow || magnitude > (unsigned long long)LLONG_MAX + (negative ? 1ull : 0ull)) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }

    return (long long)magnitude;
}

unsigned long strtoul(const char *text, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value = convert_whole(text, end, base, &negative, &overflow);

    if (overflow || value > (unsigned long long)ULONG_MAX) {
        errno = ERANGE;
        return ULONG_MAX;
    }

    return negative ? (unsigned long)0 - (unsigned long)value : (unsigned long)value;
}

long strtol(const char *text, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long long value = convert_whole(text, end, base, &negative, &overflow);
    unsigned long long magnitude = negative ? (unsigned long long)0 - value : value;

    if (overflow || magnitude > (unsigned long long)LONG_MAX + (negative ? 1ull : 0ull)) {
        errno = ERANGE;
        return negative ? LONG_MIN : LONG_MAX;
    }

    return (long)magnitude;
}

int atoi(const char *text)
{
    return (int)strtol(text, NULL, 10);
}

long atol(const char *text)
{
    return strtol(text, NULL, 10);
}

int abs(int value)
{
    return (value < 0) ? -value : value;
}

long labs(long value)
{
    return (value < 0) ? -value : value;
}

/*
 * The end of a program that found itself in a state it cannot go on from.
 * The C library raises SIGABRT; this system has the signal and no default
 * action that ends a program, so the call ends the task with the status the
 * shell of a Unix-like system sees for an abort.
 */
void abort(void)
{
    exit(134);
}

char *getenv(const char *name)
{
    size_t length;
    char **word;

    if (name == NULL || *name == '\0' || environ == NULL) {
        return NULL;
    }

    length = strlen(name);
    for (word = environ; *word != NULL; word++) {
        if (strncmp(*word, name, length) == 0 && (*word)[length] == '=') {
            return *word + length + 1;
        }
    }

    return NULL;
}

/*
 * The sort. A merge sort is stable and its work is bounded by n log n whatever
 * the data is, which is the promise a library code can count on; the copy it
 * merges into is asked of the arena of the program. When the arena has no room
 * the array is sorted in place by insertion, which is slower but never fails.
 */
static void merge_runs(unsigned char *base, unsigned char *copy, size_t left, size_t middle,
                       size_t right, size_t size, int (*compare)(const void *, const void *))
{
    size_t first = left;
    size_t second = middle;
    size_t out = left;

    while (first < middle && second < right) {
        if (compare(base + second * size, base + first * size) < 0) {
            memcpy(copy + out * size, base + second * size, size);
            second++;
        } else {
            memcpy(copy + out * size, base + first * size, size);
            first++;
        }
        out++;
    }

    while (first < middle) {
        memcpy(copy + out * size, base + first * size, size);
        first++;
        out++;
    }
    while (second < right) {
        memcpy(copy + out * size, base + second * size, size);
        second++;
        out++;
    }
}

static void merge_sort(unsigned char *base, unsigned char *copy, size_t count, size_t size,
                       int (*compare)(const void *, const void *))
{
    size_t width;

    for (width = 1; width < count; width *= 2) {
        size_t start;

        for (start = 0; start < count; start += 2 * width) {
            size_t middle = (start + width < count) ? start + width : count;
            size_t end = (start + 2 * width < count) ? start + 2 * width : count;

            merge_runs(base, copy, start, middle, end, size, compare);
        }
        memcpy(base, copy, count * size);
    }
}

static void insertion_sort(unsigned char *base, size_t count, size_t size,
                           int (*compare)(const void *, const void *))
{
    unsigned char *held = (unsigned char *)malloc(size);
    size_t i;

    if (held == NULL) {
        return; /* no room even for one element: the array stays as it was */
    }

    for (i = 1; i < count; i++) {
        size_t place = i;

        memcpy(held, base + i * size, size);
        while (place > 0 && compare(base + (place - 1) * size, held) > 0) {
            memcpy(base + place * size, base + (place - 1) * size, size);
            place--;
        }
        memcpy(base + place * size, held, size);
    }

    free(held);
}

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *))
{
    unsigned char *copy;

    if (base == NULL || compare == NULL || size == 0 || count < 2) {
        return;
    }

    copy = (unsigned char *)malloc(count * size);
    if (copy == NULL) {
        insertion_sort((unsigned char *)base, count, size, compare);
        return;
    }

    merge_sort((unsigned char *)base, copy, count, size, compare);
    free(copy);
}

long long atoll(const char *text)
{
    return strtoll(text, NULL, 10);
}

long long llabs(long long value)
{
    return (value < 0) ? -value : value;
}

/*
 * The search of an element of a sorted array. The compare is the one of
 * `qsort`, and the array is the caller's: half the range goes away at every
 * step, so a table of a command is found in a handful of calls.
 */
void *bsearch(const void *key, const void *array, size_t count, size_t size,
              int (*compare)(const void *, const void *))
{
    const unsigned char *base = (const unsigned char *)array;
    size_t low = 0;
    size_t high = count;

    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const void *element = base + middle * size;
        int order = compare(key, element);

        if (order == 0) {
            return (void *)element;
        }
        if (order < 0) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }

    return NULL;
}
