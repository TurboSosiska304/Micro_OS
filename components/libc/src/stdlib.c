/*
 * Micro OS — the memory of the C library.
 *
 * Every function here is a call of the ABI of the system calls: the
 * programs of the board do not touch the memory of the kernel directly.
 */
#include "libc/stdlib.h"

#include "libc/unistd.h"

#include "kernel/errno.h"
#include "kernel/ksys.h"

void *microos_malloc(size_t size)
{
    void *block = ksys_malloc((k_size_t)size);

    if (block == NULL) {
        /* Out of memory: the code of POSIX, so that errno can be checked. */
        microos_set_errno(-KERR_NOMEM);
        return NULL;
    }

    return block;
}

/*
 * realloc with the size zero gives back the block and returns NULL: the
 * kernel does the same, and the block is not lost. In every other case a
 * NULL means that the block of the caller is still his.
 */
void *microos_realloc(void *ptr, size_t size)
{
    void *block = ksys_realloc(ptr, (k_size_t)size);

    if (block == NULL && size != 0) {
        microos_set_errno(-KERR_NOMEM);
        return NULL;
    }

    return block;
}

void *microos_calloc(size_t count, size_t size)
{
    size_t total;
    void *block;

    if (count != 0 && size > (size_t)-1 / count) {
        microos_set_errno(-KERR_NOMEM); /* the multiplication would overflow */
        return NULL;
    }

    total = count * size;

    block = microos_malloc(total);
    if (block == NULL) {
        return NULL;
    }

    /* The memory of the kernel is not cleared, the program must do it. */
    {
        unsigned char *bytes = (unsigned char *)block;
        size_t i;

        for (i = 0; i < total; i++) {
            bytes[i] = 0;
        }
    }

    return block;
}

void microos_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    ksys_free(ptr);
}

/* --- the conversion of numbers --- */

/* The value of one character in the given base, -1 when it is not a digit. */
static int digit_of(int c, int base)
{
    int value;

    if (c >= '0' && c <= '9') {
        value = c - '0';
    } else if (c >= 'a' && c <= 'z') {
        value = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'Z') {
        value = c - 'A' + 10;
    } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
        return -2; /* the sign may be here, the digits may not */
    } else {
        return -1;
    }

    return (value < base) ? value : -1;
}

/*
 * The start of a conversion: the blanks, the sign and the base of the
 * text. Returns the first character that can be a digit, and fills the
 * state of the conversion.
 */
static const char *number_start(const char *text, int *base, int *negative, int *any)
{
    const char *p = text;

    *negative = 0;
    *any = 0;

    while (digit_of(*p, 36) == -2) {
        p++;
    }

    if (*p == '+' || *p == '-') {
        *negative = (*p == '-');
        p++;
    }

    if ((*base == 0 || *base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        if (digit_of(p[2], 16) >= 0) {
            p += 2;
            *base = 16;
        }
    } else if (*base == 0) {
        *base = (p[0] == '0') ? 8 : 10;
    }

    if (*base < 2 || *base > 36) {
        return text; /* the base is not a base: no conversion is done */
    }

    return p;
}

/*
 * The shared core: the digits of the text are collected into an unsigned
 * value. The sign and the overflow come out separately, because the
 * signed conversion has to decide on its own what to do with them.
 */
static unsigned long parse_number(const char *text, char **end, int base, int *out_negative,
                                  int *out_overflow)
{
    unsigned long limit = (unsigned long)-1;
    unsigned long value = 0;
    const char *p;
    int negative;
    int any;
    int overflow = 0;
    int saved_base = base;

    *out_negative = 0;
    *out_overflow = 0;

    if (text == NULL) {
        if (end != NULL) {
            *end = NULL;
        }
        return 0;
    }

    p = number_start(text, &saved_base, &negative, &any);

    for (;;) {
        int digit = digit_of(*p, saved_base);

        if (digit < 0) {
            break;
        }

        any = 1;

        if (value > (limit - (unsigned long)digit) / (unsigned long)saved_base) {
            overflow = 1; /* the digits go on, the value does not */
        } else {
            value = value * (unsigned long)saved_base + (unsigned long)digit;
        }

        p++;
    }

    if (end != NULL) {
        *end = (char *)(any ? p : text);
    }

    *out_negative = negative;
    *out_overflow = overflow;

    return value;
}

unsigned long microos_strtoul(const char *text, char **end, int base)
{
    int negative;
    int overflow;
    unsigned long value = parse_number(text, end, base, &negative, &overflow);

    if (overflow) {
        /* The biggest number of the type, the way the system library
         * does it, and the code of the range for the caller. */
        microos_set_errno(MICROOS_ERANGE);
        return (unsigned long)-1;
    }

    return negative ? (unsigned long)(0 - value) : value;
}

long microos_strtol(const char *text, char **end, int base)
{
    unsigned long positive_max = (unsigned long)((~0UL) >> 1); /* LONG_MAX */
    int negative;
    int overflow;
    unsigned long value = parse_number(text, end, base, &negative, &overflow);

    if (overflow) {
        microos_set_errno(MICROOS_ERANGE);
        return negative ? (long)(0 - (positive_max + 1u)) : (long)positive_max;
    }

    if (negative) {
        /* The smallest long is one further away from zero than the
         * biggest one. */
        if (value > positive_max + 1u) {
            microos_set_errno(MICROOS_ERANGE);
            return (long)(0 - (positive_max + 1u));
        }

        return (long)(0 - value);
    }

    if (value > positive_max) {
        microos_set_errno(MICROOS_ERANGE);
        return (long)positive_max;
    }

    return (long)value;
}
