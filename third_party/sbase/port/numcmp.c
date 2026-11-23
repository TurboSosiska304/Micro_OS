/*
 * Micro OS — `sort -n`, the decimal order of two numbers without a `double`.
 *
 * A program of this system is built with `-mgeneral-regs-only`: the kernel of
 * the x86-64 port saves no state of the floating point unit, so a `double`
 * cannot be returned from a function of the guest and upstream `sort` cannot
 * compile as it is. The comparison is done on the text of the number: the
 * sign, the digits and the exponent are read as a decimal and the two numbers
 * are ordered by their magnitude and then by their digits. It is the order of
 * the numbers, and it is exact: `sort -n` of a file of the machine never
 * rounds.
 *
 * A number is kept as its digits and the power of ten of their last digit:
 * `12.5` is the digits `125` and the exponent -1. The magnitude is ordered by
 * the power of ten of the first digit (the exponent plus the count) and then
 * by the digits, padded on the right with zeros, so `1`, `1.0` and `1.00` are
 * the same number and `2` is after `10` only when the first digit says so.
 * A text that is not a number counts as zero, which is what `sort -n` does
 * with a word.
 */
#include "sbase_port.h"

#include <ctype.h>
#include <string.h>

struct decimal {
    int negative;
    int count;
    int exponent; /* the power of ten of digits[count - 1] */
    char digits[64];
};

static void decimal_read(const char *text, struct decimal *out)
{
    const char *place = text;
    int fraction_digits = 0;
    long exponent = 0;
    int exponent_negative = 0;
    int any = 0;

    memset(out, 0, sizeof(*out));

    while (isspace((unsigned char)*place)) {
        place++;
    }
    if (*place == '+' || *place == '-') {
        out->negative = (*place == '-');
        place++;
    }

    while (isdigit((unsigned char)*place)) {
        if (!(out->count == 0 && *place == '0') && out->count < (int)sizeof(out->digits)) {
            out->digits[out->count++] = *place;
        }
        any = 1;
        place++;
    }

    if (*place == '.') {
        place++;
        while (isdigit((unsigned char)*place)) {
            if (out->count < (int)sizeof(out->digits)) {
                out->digits[out->count++] = *place;
            }
            fraction_digits++;
            any = 1;
            place++;
        }
    }

    if (*place == 'e' || *place == 'E') {
        const char *mark = place;

        place++;
        if (*place == '+' || *place == '-') {
            exponent_negative = (*place == '-');
            place++;
        }
        if (isdigit((unsigned char)*place)) {
            while (isdigit((unsigned char)*place)) {
                if (exponent < 100000) {
                    exponent = exponent * 10 + (*place - '0');
                }
                place++;
            }
            exponent = exponent_negative ? -exponent : exponent;
        } else {
            place = mark;
        }
    }

    if (!any) {
        out->count = 0;
        out->negative = 0;

        return;
    }

    /* The leading zeros of the integer part are not digits; the fraction
     * after them is, and the count of the integer digits with the exponent is
     * the place of the point. */
    {
        int first = 0;

        while (first < out->count && out->digits[first] == '0') {
            first++;
        }
        if (first > 0) {
            memmove(out->digits, out->digits + first, (size_t)(out->count - first));
            out->count -= first;
        }
    }

    out->exponent = (int)exponent - fraction_digits;

    if (out->count == 0) {
        out->negative = 0;
        out->exponent = 0;
    }
}

/* The power of ten of the first digit: the order of the magnitude. */
static int decimal_order(const struct decimal *number)
{
    return number->exponent + number->count - 1;
}

static int decimal_cmp(const struct decimal *left, const struct decimal *right)
{
    int order;

    if (left->count == 0 && right->count == 0) {
        return 0;
    }
    if (left->count == 0) {
        return right->negative ? 1 : -1;
    }
    if (right->count == 0) {
        return left->negative ? -1 : 1;
    }
    if (left->negative != right->negative) {
        return left->negative ? -1 : 1;
    }

    order = 0;
    if (decimal_order(left) != decimal_order(right)) {
        order = (decimal_order(left) < decimal_order(right)) ? -1 : 1;
    } else {
        int count = (left->count > right->count) ? left->count : right->count;

        for (int i = 0; i < count && order == 0; i++) {
            char a = (i < left->count) ? left->digits[i] : '0';
            char b = (i < right->count) ? right->digits[i] : '0';

            if (a != b) {
                order = (a < b) ? -1 : 1;
            }
        }
    }

    return left->negative ? -order : order;
}

int sbase_numcmp(const char *left, const char *right)
{
    struct decimal a;
    struct decimal b;

    decimal_read(left, &a);
    decimal_read(right, &b);

    return decimal_cmp(&a, &b);
}
