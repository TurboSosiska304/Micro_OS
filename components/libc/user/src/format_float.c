/*
 * Micro OS — the number with a point, as text.
 *
 * The conversion of a double to the digits of `%f`, `%e` and `%g` is not a
 * call of the formatter of the host: the image of a program carries its own.
 * The work is integer arithmetic on the parts of the value - the mantissa,
 * the exponent and the fraction - so the file never asks the x87 unit for a
 * logarithm and never loops on a value that is not a number.
 *
 * The functions produce the text of the number alone, sign included and width
 * excluded: the padding of a field is the business of the formatter, which
 * already does it for the whole numbers and the strings. `stdio.c` calls
 * `ulibc_double_text` and pads the answer.
 *
 * This file is built only for a program that computes in floating point
 * (`ULIBC_WITH_FLOAT`): a program with `-mgeneral-regs-only` has no register
 * for a double and prints the conversion as it was written.
 */
#include "ulibc.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "format_float.h"

#define NUM_BUF 64
#define TEXT_MAX 512

struct number {
    int precision;
    int plus;
    int space;
    int alternate;
};

/* The digits of the fraction, with the rounding of the last one. */
static int fraction_digits(double *fraction, char *digits, int precision)
{
    int count = 0;
    int i;

    for (i = 0; i < precision && count < NUM_BUF - 2; i++) {
        int digit;

        *fraction *= 10.0;
        digit = (int)*fraction;
        if (digit < 0) {
            digit = 0;
        }
        if (digit > 9) {
            digit = 9;
        }
        digits[count++] = (char)('0' + digit);
        *fraction -= (double)digit;
    }

    if (*fraction >= 0.5 - 1e-9) {
        int place = count - 1;

        while (place >= 0) {
            if (digits[place] < '9') {
                digits[place]++;
                break;
            }
            digits[place] = '0';
            place--;
        }
        if (place < 0) {
            digits[0] = '1';
            *fraction = 0.0;
            return -1; /* the carry goes to the integer part */
        }
    }

    return count;
}

static int is_negative(double value)
{
    uint64_t bits = 0;

    memcpy(&bits, &value, sizeof(bits));

    return (bits >> 63) != 0u;
}

static int special_text(char *out, size_t cap, const struct number *sp, double value)
{
    static const double biggest = 1.7976931348623157e308;
    const char *word;
    char text[16];
    int length = 0;

    if (value == value) {
        if (value <= biggest && value >= -biggest) {
            return -1;
        }
        word = "inf";
    } else {
        word = "nan";
    }

    if (is_negative(value)) {
        text[length++] = '-';
    } else if (sp->plus) {
        text[length++] = '+';
    } else if (sp->space) {
        text[length++] = ' ';
    }

    memcpy(text + length, word, 3);
    length += 3;
    text[length] = '\0';

    if ((size_t)length >= cap) {
        length = (int)cap - 1;
    }
    memcpy(out, text, (size_t)length);
    out[length] = '\0';

    return length;
}

/*
 * The integer part of a number that is too big for the whole numbers of the
 * machine. The value of a double is exact, and a number of this size is a
 * whole one: it is written out through groups of nine digits, the way a big
 * integer is written out.
 */
#define BIG_WORDS 40

static int big_integer_text(char *out, size_t cap, double value)
{
    uint64_t bits = 0;
    uint32_t words[BIG_WORDS];
    uint64_t mantissa;
    int count;
    int shift;
    int biased;
    int length = 0;
    int i;

    memcpy(&bits, &value, sizeof(bits));
    biased = (int)((bits >> 52) & 0x7FFu);
    mantissa = (bits & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull;
    shift = biased - 1023 - 52;

    words[0] = (uint32_t)(mantissa % 1000000000u);
    words[1] = (uint32_t)(mantissa / 1000000000u);
    count = (words[1] != 0) ? 2 : 1;

    for (i = 0; i < shift; i++) {
        uint32_t carry = 0;
        int j;

        for (j = 0; j < count; j++) {
            uint32_t piece = words[j] * 2u + carry;

            words[j] = piece % 1000000000u;
            carry = piece / 1000000000u;
        }

        if (carry != 0 && count < BIG_WORDS) {
            words[count++] = carry;
        }
    }

    {
        uint32_t top = words[count - 1];
        char piece[10];
        int n = 0;

        do {
            piece[n++] = (char)('0' + (int)(top % 10u));
            top /= 10u;
        } while (top != 0 && n < (int)sizeof(piece));

        while (n > 0 && length < (int)cap) {
            out[length++] = piece[--n];
        }
    }

    for (i = count - 2; i >= 0; i--) {
        uint32_t top = words[i];
        char piece[9];
        int n = 9;

        while (n > 0) {
            piece[--n] = (char)('0' + (int)(top % 10u));
            top /= 10u;
        }

        n = 0;
        while (n < 9 && length < (int)cap) {
            out[length++] = piece[n++];
        }
    }

    if (length < (int)cap) {
        out[length] = '\0';
    }

    return length;
}

static int whole_text(char *out, size_t cap, unsigned long long value)
{
    char digits[32];
    int count = 0;
    int length = 0;

    if (value == 0) {
        digits[count++] = '0';
    }
    while (value > 0) {
        digits[count++] = (char)('0' + (int)(value % 10ull));
        value /= 10ull;
    }
    while (count > 0 && length < (int)cap) {
        out[length++] = digits[--count];
    }
    if (length < (int)cap) {
        out[length] = '\0';
    }

    return length;
}

/* The number with a point: %f. */
static int fixed_text(char *out, size_t cap, const struct number *sp, double value)
{
    char digits[NUM_BUF];
    char text[TEXT_MAX];
    int precision = sp->precision;
    int negative = 0;
    unsigned long long whole;
    double fraction;
    int count;
    int length = 0;
    int i;

    {
        int special = special_text(out, cap, sp, value);

        if (special >= 0) {
            return special;
        }
    }

    if (is_negative(value)) {
        negative = 1;
        value = -value;
    }

    if (negative) {
        text[length++] = '-';
    } else if (sp->plus) {
        text[length++] = '+';
    } else if (sp->space) {
        text[length++] = ' ';
    }

    if (value >= 18446744073709551616.0) {
        /* 2^64 and more: the fraction is zero and the whole part is written
         * through the big integer. */
        count = 0;
        if (precision > 0) {
            count = (precision < NUM_BUF - 2) ? precision : (NUM_BUF - 2);
            memset(digits, '0', (size_t)count);
        }
        length +=
            big_integer_text(text + length, sizeof(text) - (size_t)length - (size_t)NUM_BUF, value);
    } else {
        whole = (unsigned long long)value;
        fraction = value - (double)whole;

        count = fraction_digits(&fraction, digits, precision);
        if (count < 0) {
            whole++;
            count = 0;
        }

        length += whole_text(text + length, sizeof(text) - (size_t)length, whole);
    }

    if (precision > 0 || sp->alternate) {
        text[length++] = '.';
    }
    for (i = 0; i < count; i++) {
        text[length++] = digits[i];
    }
    text[length] = '\0';

    if ((size_t)length >= cap) {
        length = (int)cap - 1;
    }
    memcpy(out, text, (size_t)length);
    out[length] = '\0';

    return length;
}

/* The number with an exponent: %e. */
static int exponent_text(char *out, size_t cap, const struct number *sp, double value, int upper)
{
    char digits[NUM_BUF];
    char text[NUM_BUF + 32];
    int precision = sp->precision;
    int negative = 0;
    int exponent = 0;
    unsigned long long whole;
    double fraction;
    int count;
    int length = 0;
    int i;
    int guard;

    {
        int special = special_text(out, cap, sp, value);

        if (special >= 0) {
            return special;
        }
    }

    if (is_negative(value)) {
        negative = 1;
        value = -value;
    }

    if (value != 0.0) {
        for (guard = 0; guard < 400 && value >= 10.0; guard++) {
            value /= 10.0;
            exponent++;
        }
        for (guard = 0; guard < 400 && value < 1.0 && value != 0.0; guard++) {
            value *= 10.0;
            exponent--;
        }
    }

    whole = (unsigned long long)value;
    fraction = value - (double)whole;

    count = fraction_digits(&fraction, digits, precision);
    if (count < 0) {
        whole++;
        if (whole >= 10) {
            whole = 1;
            exponent++;
        }
        count = 0;
    }

    if (negative) {
        text[length++] = '-';
    } else if (sp->plus) {
        text[length++] = '+';
    } else if (sp->space) {
        text[length++] = ' ';
    }

    text[length++] = (char)('0' + (int)whole);

    if (precision > 0 || sp->alternate) {
        text[length++] = '.';
    }
    for (i = 0; i < count; i++) {
        text[length++] = digits[i];
    }

    text[length++] = upper ? 'E' : 'e';
    text[length++] = exponent < 0 ? '-' : '+';
    {
        int absolute = exponent < 0 ? -exponent : exponent;

        if (absolute >= 100) {
            text[length++] = (char)('0' + (absolute / 100));
        }
        text[length++] = (char)('0' + ((absolute / 10) % 10));
        text[length++] = (char)('0' + (absolute % 10));
    }
    text[length] = '\0';

    if ((size_t)length >= cap) {
        length = (int)cap - 1;
    }
    memcpy(out, text, (size_t)length);
    out[length] = '\0';

    return length;
}

/* The shortest of the two forms: %g. */
static int general_text(char *out, size_t cap, const struct number *sp, double value, int upper)
{
    struct number copy = *sp;
    int precision = sp->precision;
    int exponent = 0;
    double absolute = (value < 0.0) ? -value : value;
    char text[NUM_BUF + 32];
    char *point;
    char *mark;
    size_t end;
    size_t last;
    int length;
    int guard;

    {
        int special = special_text(out, cap, sp, value);

        if (special >= 0) {
            return special;
        }
    }

    if (precision == 0) {
        precision = 1;
    }

    if (absolute != 0.0) {
        for (guard = 0; guard < 400 && absolute >= 10.0; guard++) {
            absolute /= 10.0;
            exponent++;
        }
        for (guard = 0; guard < 400 && absolute < 1.0 && absolute != 0.0; guard++) {
            absolute *= 10.0;
            exponent--;
        }
    }

    copy.precision = precision;
    if (exponent < -4 || exponent >= precision) {
        copy.precision = precision - 1;
        length = exponent_text(text, sizeof(text), &copy, value, upper);
    } else {
        copy.precision = precision - 1 - exponent;
        length = fixed_text(text, sizeof(text), &copy, value);
    }

    end = (size_t)length;
    mark = NULL;
    {
        int i;

        for (i = 0; i < length; i++) {
            if (text[i] == 'e' || text[i] == 'E') {
                mark = &text[i];
                end = (size_t)i;
                break;
            }
        }
    }

    point = NULL;
    {
        size_t i;

        for (i = 0; i < end; i++) {
            if (text[i] == '.') {
                point = &text[i];
                break;
            }
        }
    }

    if (point != NULL && !sp->alternate) {
        size_t at = (size_t)(point - text);

        last = end;
        while (last > at + 1 && text[last - 1] == '0') {
            last--;
        }

        if (mark != NULL) {
            size_t tail = (size_t)length - end;

            if (last == at + 1) {
                memmove(text + at, text + end, tail);
                length = (int)(at + tail);
            } else {
                memmove(text + last, text + end, tail);
                length = (int)(last + tail);
            }
        } else {
            length = (last == at + 1) ? (int)at : (int)last;
        }
    }

    if ((size_t)length >= cap) {
        length = (int)cap - 1;
    }
    memcpy(out, text, (size_t)length);
    out[length] = '\0';

    return length;
}

int ulibc_double_text(char *out, size_t cap, double value, char conversion, int precision, int plus,
                      int space, int alternate)
{
    struct number sp;
    int upper = (conversion >= 'A' && conversion <= 'Z');

    if (cap == 0) {
        return 0;
    }

    sp.precision = (precision < 0) ? 6 : precision;
    if (sp.precision > NUM_BUF - 4) {
        sp.precision = NUM_BUF - 4;
    }
    sp.plus = plus;
    sp.space = space;
    sp.alternate = alternate;

    switch (conversion) {
    case 'f':
    case 'F':
        return fixed_text(out, cap, &sp, value);
    case 'e':
    case 'E':
        return exponent_text(out, cap, &sp, value, upper);
    default:
        return general_text(out, cap, &sp, value, upper);
    }
}
