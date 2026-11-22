/*
 * Micro OS — the number with a point, read from its text.
 *
 * This is the conversion of `feature/lua` (`src/stdlib.c` there), brought
 * here in a file of its own because the rest of that file is a second
 * implementation of the whole numbers this tree already has. The guest has
 * no libm of the host under it: a program that reads a real number
 * (SQLite's `strtod()` SQL function, a shell argument) needs the conversion
 * in its own image.
 *
 * The form is the decimal one of the C locale - an optional sign, digits, a
 * point and an optional exponent. The value is accumulated as a whole number
 * of up to nineteen digits and scaled by the exact powers of ten a double
 * holds; the last bit of a long text is therefore the rounding of the
 * machine, not of a decimal library (`docs/debts.md`).
 */
#include "ulibc.h"

#include <errno.h>
#include <math.h>

static int space_byte(int byte)
{
    return byte == ' ' || (byte >= '\t' && byte <= '\r');
}

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
