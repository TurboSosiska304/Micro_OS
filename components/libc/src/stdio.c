/*
 * Micro OS — the formatting of the C library.
 *
 * A small formatter: the formats of the C library, without the parts that
 * the programs of the board do not need. It writes either into a buffer
 * (snprintf) or to a descriptor through the ABI of the system calls.
 */
#include "libc/stdio.h"

#include "libc/unistd.h"

#include <stdint.h>
#include <string.h>

/* The size of the piece that goes to the console at once. */
#define OUT_CHUNK 256

/* The size of the buffers of the numbers. */
#define NUM_BUF 72

/*
 * The size of the buffer of a number with a point. The biggest double
 * has 309 digits before the point, and %f prints all of them.
 */
#define FMT_TEXT_MAX 400

/* The description of one conversion. */
struct spec {
    int left;  /* '-' */
    int plus;  /* '+' */
    int space; /* ' ' */
    int alt;   /* '#' */
    int zero;  /* '0' */
    int width;
    int precision; /* -1 when it is not given */
    int length;    /* 0 int, 1 long, 2 long long, 3 size_t, 4 short */
};

/* Where the characters go. */
struct sink {
    char *buf;      /* the buffer of snprintf, NULL for the console */
    size_t cap;     /* the size of the buffer */
    size_t written; /* the number of the characters in the buffer */
    size_t total;   /* the number of the characters, even the cut ones */
    char chunk[OUT_CHUNK];
    size_t chunk_len;
    int fd;
    int error; /* the error of the last write */
};

static void sink_init_buf(struct sink *s, char *buf, size_t size)
{
    s->buf = buf;
    s->cap = size;
    s->written = 0;
    s->total = 0;
    s->chunk_len = 0;
    s->fd = -1;
    s->error = 0;

    if (buf != NULL && size > 0) {
        buf[0] = '\0';
    }
}

static void sink_init_fd(struct sink *s, int fd)
{
    s->buf = NULL;
    s->cap = 0;
    s->written = 0;
    s->total = 0;
    s->chunk_len = 0;
    s->fd = fd;
    s->error = 0;
}

static void sink_flush(struct sink *s)
{
    if (s->chunk_len == 0) {
        return;
    }

    if (s->fd >= 0) {
        long rc = microos_write(s->fd, s->chunk, s->chunk_len);

        if (rc < 0) {
            s->error = (int)rc;
        }
    }

    s->chunk_len = 0;
}

static void sink_putc(struct sink *s, char c)
{
    s->total++;

    if (s->buf != NULL) {
        if (s->cap > 1 && s->written < s->cap - 1) {
            s->buf[s->written] = c;
        }
        s->written++;
        return;
    }

    s->chunk[s->chunk_len++] = c;
    if (s->chunk_len == OUT_CHUNK) {
        sink_flush(s);
    }
}

static void sink_puts(struct sink *s, const char *text, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        sink_putc(s, text[i]);
    }
}

/* The end of the formatting: the zero of the buffer, the rest to the console. */
static int sink_finish(struct sink *s)
{
    if (s->buf != NULL) {
        if (s->cap > 0) {
            s->buf[(s->written < s->cap - 1) ? s->written : (s->cap - 1)] = '\0';
        }
    } else {
        sink_flush(s);
    }

    if (s->error != 0) {
        return s->error;
    }

    return (int)s->total;
}

/* The number into a string, in the given base. Returns the length. */
static int format_utoa(char *out, unsigned long long value, unsigned base, int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[NUM_BUF];
    int n = 0;
    int i;

    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value != 0 && n < (int)sizeof(tmp));

    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = '\0';

    return n;
}

/*
 * The number with its sign, its prefix and its padding. The zeros of the
 * width come after the sign, the way the C library does it.
 */
static void put_number(struct sink *s, const struct spec *sp, int negative,
                       unsigned long long value, unsigned base, int upper)
{
    char digits[NUM_BUF];
    char prefix[3];
    int ndigits;
    int nprefix = 0;
    int sign = 0;
    int zeros = 0;
    int use_zero;
    int pad;
    int i;

    ndigits = format_utoa(digits, value, base, upper);

    /* The precision of the numbers is the minimum of the digits. */
    if (sp->precision > ndigits) {
        int extra = sp->precision - ndigits;

        if (extra > (int)sizeof(digits) - 2) {
            extra = (int)sizeof(digits) - 2;
        }
        memmove(digits + extra, digits, (size_t)ndigits);
        for (i = 0; i < extra; i++) {
            digits[i] = '0';
        }
        ndigits += extra;
    }

    if (sp->alt && value != 0) {
        if (base == 16) {
            prefix[nprefix++] = '0';
            prefix[nprefix++] = upper ? 'X' : 'x';
        } else if (base == 8) {
            prefix[nprefix++] = '0';
        }
    }
    prefix[nprefix] = '\0';

    if (negative) {
        sign = 1;
    } else if (sp->plus) {
        sign = 2; /* '+' */
    } else if (sp->space) {
        sign = 3; /* ' ' */
    }

    /*
     * The zeros of the width are not used when the precision is given:
     * the precision already decides the digits, the rest is the padding
     * of the C library - spaces.
     */
    use_zero = sp->zero && sp->precision < 0;

    /* The padding in front, when the number is on the right. */
    if (!sp->left && !use_zero) {
        pad = sp->width - ndigits - nprefix - (sign ? 1 : 0);
        while (pad-- > 0) {
            sink_putc(s, ' ');
        }
    }

    if (sign == 1) {
        sink_putc(s, '-');
    } else if (sign == 2) {
        sink_putc(s, '+');
    } else if (sign == 3) {
        sink_putc(s, ' ');
    }

    sink_puts(s, prefix, (size_t)nprefix);

    /* The zeros of the width, when they are asked for. */
    if (!sp->left && use_zero) {
        zeros = sp->width - ndigits - nprefix - (sign ? 1 : 0);
        while (zeros-- > 0) {
            sink_putc(s, '0');
        }
    }

    sink_puts(s, digits, (size_t)ndigits);

    /* The padding behind, when the number is on the left. */
    if (sp->left) {
        pad = sp->width - ndigits - nprefix - (sign ? 1 : 0);
        while (pad-- > 0) {
            sink_putc(s, ' ');
        }
    }
}

/* The digits of the fraction, with the rounding of the last one. */
static int fraction_digits(double *fp, char *digits, int precision)
{
    int n = 0;
    int i;

    for (i = 0; i < precision && n < NUM_BUF - 2; i++) {
        int d;

        *fp *= 10.0;
        d = (int)*fp;
        if (d < 0) {
            d = 0;
        }
        if (d > 9) {
            d = 9;
        }
        digits[n++] = (char)('0' + d);
        *fp -= (double)d;
    }

    /* The rounding: the rest of the fraction decides. */
    if (*fp >= 0.5 - 1e-9) {
        int j = n - 1;

        while (j >= 0) {
            if (digits[j] < '9') {
                digits[j]++;
                break;
            }
            digits[j] = '0';
            j--;
        }
        if (j < 0) {
            digits[0] = '1';
            n = 1;
            *fp = 0.0;
            return -1; /* the carry goes to the integer part */
        }
    }

    return n;
}

/* The text with its padding. */
static void emit_text(struct sink *s, const struct spec *sp, const char *text, int len)
{
    int pad = sp->width - len;
    int skip = 0;
    int zeros = 0;

    /*
     * The zeros of the width of a number with a point come after the sign,
     * the way the C library does it. The flag is not used when the text is
     * on the left.
     */
    if (!sp->left && sp->zero && pad > 0) {
        if (len > 0 && (text[0] == '-' || text[0] == '+' || text[0] == ' ')) {
            skip = 1;
        }
        zeros = pad;
        pad = 0;
    }

    if (!sp->left) {
        while (pad-- > 0) {
            sink_putc(s, ' ');
        }
    }

    if (skip != 0) {
        sink_putc(s, text[0]);
        text++;
        len--;
    }

    while (zeros-- > 0) {
        sink_putc(s, '0');
    }

    sink_puts(s, text, (size_t)len);

    if (sp->left) {
        pad = sp->width - len;
        while (pad-- > 0) {
            sink_putc(s, ' ');
        }
    }
}

/*
 * The sign of a number, with the minus zero: -0.0 is a number with a
 * sign, and the C library prints it as "-0.000000".
 */
static int is_negative(double value)
{
    uint64_t bits = 0;

    memcpy(&bits, &value, sizeof(bits));

    return (bits >> 63) != 0u;
}

/*
 * The values that are not numbers: the infinity and the not-a-number of
 * the hardware. The C library prints them as words; a formatter that
 * counted the digits of an infinite number would never come back.
 * Returns the number of the characters, or -1 when the value is a real
 * number and the caller has to go on.
 */
static int special_text(char *out, size_t cap, const struct spec *sp, double value)
{
    static const double double_max = 1.7976931348623157e308; /* the biggest double */
    const char *word;
    char text[16];
    int len = 0;

    if (value == value) {
        /* Not a NaN: the infinity is the only value left. */
        if (value <= double_max && value >= -double_max) {
            return -1;
        }
        word = "inf";
    } else {
        word = "nan";
    }

    if (value < 0.0) {
        text[len++] = '-';
    } else if (sp->plus) {
        text[len++] = '+';
    } else if (sp->space) {
        text[len++] = ' ';
    }

    memcpy(text + len, word, 3);
    len += 3;
    text[len] = '\0';

    if ((size_t)len >= cap) {
        len = (int)cap - 1;
    }
    memcpy(out, text, (size_t)len);
    out[len] = '\0';

    return len;
}

/*
 * The integer part of a number that is too big for the integers of the
 * machine. The value of a double is exact, and for a number of this size
 * it is a whole number: it is written out through a small array of
 * digits, the way a big integer is written out. The text comes without a
 * sign and without the fraction - the fraction of such a number is zero.
 */
#define BIG_WORDS 40 /* 40 groups of nine digits: more than any double */

static int big_integer_text(char *out, size_t cap, double value)
{
    uint64_t bits = 0;
    uint32_t words[BIG_WORDS]; /* the groups, the least significant first */
    uint64_t mantissa;
    int count;
    int shift;
    int biased;
    int len = 0;
    int i;

    memcpy(&bits, &value, sizeof(bits));
    biased = (int)((bits >> 52) & 0x7FFu);
    mantissa = (bits & 0xFFFFFFFFFFFFFull) | 0x10000000000000ull; /* the hidden bit */
    shift = biased - 1023 - 52; /* the value is mantissa * 2^shift */

    /* The mantissa is smaller than 2^53: two groups of nine digits hold it. */
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

    /* The most significant group, without the zeros in front of it. */
    {
        uint32_t top = words[count - 1];
        char piece[10];
        int n = 0;

        do {
            piece[n++] = (char)('0' + (int)(top % 10u));
            top /= 10u;
        } while (top != 0 && n < (int)sizeof(piece));

        while (n > 0 && len < (int)cap) {
            out[len++] = piece[--n];
        }
    }

    /* The rest of the groups, always nine digits. */
    for (i = count - 2; i >= 0; i--) {
        uint32_t top = words[i];
        char piece[9];
        int n = 9;

        while (n > 0) {
            piece[--n] = (char)('0' + (int)(top % 10u));
            top /= 10u;
        }

        n = 0;
        while (n < 9 && len < (int)cap) {
            out[len++] = piece[n++];
        }
    }

    if (len < (int)cap) {
        out[len] = '\0';
    }

    return len;
}

/* The number with a point: %f. The text goes into the buffer. */
static int fixed_text(char *out, size_t cap, const struct spec *sp, double value)
{
    char digits[NUM_BUF];
    char text[FMT_TEXT_MAX];
    int precision = (sp->precision < 0) ? 6 : sp->precision;
    int negative = 0;
    unsigned long long ip;
    double fp;
    int ndigits;
    int len = 0;
    int i;

    /* The infinity and the NaN are words, not digits. */
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
        text[len++] = '-';
    } else if (sp->plus) {
        text[len++] = '+';
    } else if (sp->space) {
        text[len++] = ' ';
    }

    if (value >= 18446744073709551616.0) {
        /* 2^64 and more: the integers of the machine cannot hold it. A
         * double this big is a whole number: the fraction is zero, and
         * the space for the point and the fraction is kept. */
        ndigits = 0;
        if (precision > 0) {
            ndigits = (precision < NUM_BUF - 2) ? precision : (NUM_BUF - 2);
            memset(digits, '0', (size_t)ndigits);
        }

        len += big_integer_text(text + len,
                                sizeof(text) - (size_t)len - 1u - (size_t)NUM_BUF, value);
    } else {
        ip = (unsigned long long)value;
        fp = value - (double)ip;

        ndigits = fraction_digits(&fp, digits, precision);
        if (ndigits < 0) {
            ip++;
            ndigits = 0;
        }

        len += format_utoa(text + len, ip, 10, 0);
    }

    if (precision > 0) {
        text[len++] = '.';
        for (i = 0; i < ndigits; i++) {
            text[len++] = digits[i];
        }
    }
    text[len] = '\0';

    if ((size_t)len >= cap) {
        len = (int)cap - 1;
    }
    memcpy(out, text, (size_t)len);
    out[len] = '\0';

    return len;
}

/* The number with an exponent: %e. The text goes into the buffer. */
static int exponent_text(char *out, size_t cap, const struct spec *sp, double value, int upper)
{
    char digits[NUM_BUF];
    char text[NUM_BUF + 32];
    int precision = (sp->precision < 0) ? 6 : sp->precision;
    int negative = 0;
    int exponent = 0;
    unsigned long long ip;
    double fp;
    int ndigits;
    int len = 0;
    int i;
    int guard;

    /* The infinity and the NaN are words, not digits. */
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

    /* The loops are bounded: the value is a real number, but a formatter
     * must never hang whatever it is given. */
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

    /* The value zero stays zero: the system prints 0.000000e+00 for it. */
    ip = (unsigned long long)value;
    fp = value - (double)ip;

    ndigits = fraction_digits(&fp, digits, precision);
    if (ndigits < 0) {
        ip++;
        if (ip >= 10) {
            ip = 1;
            exponent++;
        }
        ndigits = 0;
    }

    if (negative) {
        text[len++] = '-';
    } else if (sp->plus) {
        text[len++] = '+';
    } else if (sp->space) {
        text[len++] = ' ';
    }

    text[len++] = (char)('0' + (int)ip);

    if (precision > 0) {
        text[len++] = '.';
        for (i = 0; i < ndigits; i++) {
            text[len++] = digits[i];
        }
    }

    text[len++] = upper ? 'E' : 'e';
    text[len++] = exponent < 0 ? '-' : '+';

    {
        int abs_exp = exponent < 0 ? -exponent : exponent;

        if (abs_exp >= 100) {
            text[len++] = (char)('0' + (abs_exp / 100));
        }
        text[len++] = (char)('0' + ((abs_exp / 10) % 10));
        text[len++] = (char)('0' + (abs_exp % 10));
    }
    text[len] = '\0';

    if ((size_t)len >= cap) {
        len = (int)cap - 1;
    }
    memcpy(out, text, (size_t)len);
    out[len] = '\0';

    return len;
}

/*
 * The shortest of the two forms: %g. The zeros at the end of the fraction
 * are removed, and the point goes away when nothing is left after it.
 */
static int general_text(char *out, size_t cap, const struct spec *sp, double value, int upper)
{
    struct spec copy = *sp;
    int precision = (sp->precision < 0) ? 6 : sp->precision;
    int exponent = 0;
    double abs_value = (value < 0.0) ? -value : value;
    char text[NUM_BUF + 32];
    char *dot;
    char *mark;
    size_t end;
    size_t last;
    int len;
    int guard;

    /* The infinity and the NaN are words, not digits. */
    {
        int special = special_text(out, cap, sp, value);

        if (special >= 0) {
            return special;
        }
    }

    if (precision == 0) {
        precision = 1;
    }

    if (abs_value != 0.0) {
        for (guard = 0; guard < 400 && abs_value >= 10.0; guard++) {
            abs_value /= 10.0;
            exponent++;
        }
        for (guard = 0; guard < 400 && abs_value < 1.0 && abs_value != 0.0; guard++) {
            abs_value *= 10.0;
            exponent--;
        }
    }

    copy.precision = precision;

    if (exponent < -4 || exponent >= precision) {
        copy.precision = precision - 1;
        len = exponent_text(text, sizeof(text), &copy, value, upper);
    } else {
        copy.precision = precision - 1 - exponent;
        len = fixed_text(text, sizeof(text), &copy, value);
    }

    /* The part of the mantissa, before the exponent. */
    end = (size_t)len;
    mark = NULL;
    {
        int i;

        for (i = 0; i < len; i++) {
            if (text[i] == 'e' || text[i] == 'E') {
                mark = &text[i];
                end = (size_t)i;
                break;
            }
        }
    }

    dot = NULL;
    {
        size_t i;

        for (i = 0; i < end; i++) {
            if (text[i] == '.') {
                dot = &text[i];
                break;
            }
        }
    }

    if (dot != NULL) {
        size_t point = (size_t)(dot - text);

        last = end;

        while (last > point + 1 && text[last - 1] == '0') {
            last--;
        }

        if (mark != NULL) {
            size_t tail = (size_t)len - end; /* the exponent and what is after it */

            if (last == point + 1) {
                /* Nothing is left after the point: the point and the
                 * fraction go away, the exponent follows the digits. */
                memmove(text + point, text + end, tail);
                len = (int)(point + tail);
            } else {
                memmove(text + last, text + end, tail);
                len = (int)(last + tail);
            }
        } else {
            len = (last == point + 1) ? (int)point : (int)last;
        }
    }

    if ((size_t)len >= cap) {
        len = (int)cap - 1;
    }
    memcpy(out, text, (size_t)len);
    out[len] = '\0';

    return len;
}

static void put_general(struct sink *s, const struct spec *sp, double value, int upper)
{
    char text[FMT_TEXT_MAX];
    int len = general_text(text, sizeof(text), sp, value, upper);

    emit_text(s, sp, text, len);
}

/* The string: %s. */
static void put_string(struct sink *s, const struct spec *sp, const char *text)
{
    size_t len;

    if (text == NULL) {
        text = "(null)";
    }

    len = strlen(text);
    if (sp->precision >= 0 && len > (size_t)sp->precision) {
        len = (size_t)sp->precision;
    }

    {
        int pad = sp->width - (int)len;

        if (!sp->left) {
            while (pad-- > 0) {
                sink_putc(s, ' ');
            }
        }

        sink_puts(s, text, len);

        if (sp->left) {
            while (pad-- > 0) {
                sink_putc(s, ' ');
            }
        }
    }
}

/* The formatting itself. */
static int format(struct sink *s, const char *fmt, va_list ap)
{
    const char *p = fmt;

    while (*p != '\0') {
        struct spec sp;
        char conv;

        if (*p != '%') {
            sink_putc(s, *p++);
            continue;
        }

        p++;

        sp.left = 0;
        sp.plus = 0;
        sp.space = 0;
        sp.alt = 0;
        sp.zero = 0;
        sp.width = 0;
        sp.precision = -1;
        sp.length = 0;

        /* The flags. */
        for (;;) {
            if (*p == '-') {
                sp.left = 1;
            } else if (*p == '+') {
                sp.plus = 1;
            } else if (*p == ' ') {
                sp.space = 1;
            } else if (*p == '#') {
                sp.alt = 1;
            } else if (*p == '0') {
                sp.zero = 1;
            } else {
                break;
            }
            p++;
        }

        /* The width. */
        if (*p == '*') {
            sp.width = va_arg(ap, int);
            if (sp.width < 0) {
                sp.left = 1;
                sp.width = -sp.width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') {
                sp.width = sp.width * 10 + (*p - '0');
                p++;
            }
        }

        /* The precision. */
        if (*p == '.') {
            p++;
            sp.precision = 0;
            if (*p == '*') {
                sp.precision = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9') {
                    sp.precision = sp.precision * 10 + (*p - '0');
                    p++;
                }
            }
            if (sp.precision < 0) {
                sp.precision = -1;
            }
        }

        /* The length of the number. */
        for (;;) {
            if (*p == 'l') {
                sp.length = (sp.length == 1) ? 2 : 1;
                p++;
            } else if (*p == 'h') {
                sp.length = (sp.length == 4) ? 5 : 4;
                p++;
            } else if (*p == 'z' || *p == 'j' || *p == 't') {
                sp.length = 3;
                p++;
            } else {
                break;
            }
        }

        conv = *p;
        if (conv == '\0') {
            break;
        }
        p++;

        switch (conv) {
        case 'd':
        case 'i': {
            long long value;

            if (sp.length == 2) {
                value = va_arg(ap, long long);
            } else if (sp.length == 1 || sp.length == 3) {
                value = va_arg(ap, long);
            } else if (sp.length == 4) {
                value = (short)va_arg(ap, int); /* %hd */
            } else if (sp.length == 5) {
                value = (signed char)va_arg(ap, int); /* %hhd */
            } else {
                value = va_arg(ap, int);
            }

            if (value < 0) {
                put_number(s, &sp, 1, (unsigned long long)(-(value + 1)) + 1ULL, 10, 0);
            } else {
                put_number(s, &sp, 0, (unsigned long long)value, 10, 0);
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            unsigned long long value;

            if (sp.length == 2) {
                value = va_arg(ap, unsigned long long);
            } else if (sp.length == 1 || sp.length == 3) {
                value = va_arg(ap, unsigned long);
            } else if (sp.length == 4) {
                value = (unsigned short)va_arg(ap, unsigned int); /* %hu */
            } else if (sp.length == 5) {
                value = (unsigned char)va_arg(ap, unsigned int); /* %hhu */
            } else {
                value = va_arg(ap, unsigned int);
            }

            put_number(s, &sp, 0, value, (conv == 'u') ? 10 : ((conv == 'o') ? 8 : 16),
                       (conv == 'X'));
            break;
        }
        case 'c': {
            char ch = (char)va_arg(ap, int);
            int pad = sp.width - 1;

            if (!sp.left) {
                while (pad-- > 0) {
                    sink_putc(s, ' ');
                }
            }
            sink_putc(s, ch);
            if (sp.left) {
                while (pad-- > 0) {
                    sink_putc(s, ' ');
                }
            }
            break;
        }
        case 's':
            put_string(s, &sp, va_arg(ap, const char *));
            break;
        case 'p': {
            void *ptr = va_arg(ap, void *);
            unsigned long long value = (unsigned long long)(uintptr_t)ptr;

            sp.alt = 1;
            put_number(s, &sp, 0, value, 16, 0);
            break;
        }
        case 'f':
        case 'F': {
            char text[FMT_TEXT_MAX];
            int len = fixed_text(text, sizeof(text), &sp, va_arg(ap, double));

            emit_text(s, &sp, text, len);
            break;
        }
        case 'e':
        case 'E': {
            char text[FMT_TEXT_MAX];
            int len = exponent_text(text, sizeof(text), &sp, va_arg(ap, double), conv == 'E');

            emit_text(s, &sp, text, len);
            break;
        }
        case 'g':
        case 'G':
            put_general(s, &sp, va_arg(ap, double), conv == 'G');
            break;
        case '%':
            sink_putc(s, '%');
            break;
        default:
            sink_putc(s, '%');
            sink_putc(s, conv);
            break;
        }
    }

    return sink_finish(s);
}

int microos_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s;

    sink_init_buf(&s, buf, size);

    return format(&s, fmt, ap);
}

int microos_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = microos_vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return rc;
}

int microos_vprintf(const char *fmt, va_list ap)
{
    struct sink s;

    sink_init_fd(&s, 1);

    return format(&s, fmt, ap);
}

int microos_printf(const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = microos_vprintf(fmt, ap);
    va_end(ap);

    return rc;
}

int microos_fprintf(int fd, const char *fmt, ...)
{
    struct sink s;
    va_list ap;
    int rc;

    sink_init_fd(&s, fd);

    va_start(ap, fmt);
    rc = format(&s, fmt, ap);
    va_end(ap);

    return rc;
}

int microos_puts(const char *text)
{
    struct sink s;

    sink_init_fd(&s, 1);

    if (text == NULL) {
        return -1;
    }

    sink_puts(&s, text, strlen(text));

    return sink_finish(&s);
}

int microos_putchar(int ch)
{
    struct sink s;

    sink_init_fd(&s, 1);
    sink_putc(&s, (char)ch);

    return sink_finish(&s);
}
