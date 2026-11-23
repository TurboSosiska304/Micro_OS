/*
 * Micro OS — the formatter and the console of a program.
 *
 * The output goes through write(1, ...) in pieces of a fixed buffer: a
 * program has no allocator of its own, and a formatter that asked the kernel
 * for memory would make every printf a call. The buffer is small because the
 * stack of a program is 64 KiB and a line of this system is short.
 *
 * The conversions are the ones a program of the tree uses - %d, %i, %u, %o,
 * %x, %X, %c, %s, %p and %% - with a width, a precision, the flags '-' '+' ' '
 * '#' '0' and a '*' that takes the width or the precision from the arguments.
 * A build that defines ULIBC_WITH_FLOAT adds the conversions of a double, and
 * the digits of them come from src/format_float.c: this file parses the
 * conversion and pads the field, that one writes the number.
 *
 * A build without ULIBC_WITH_FLOAT is compiled with -mgeneral-regs-only,
 * where a double has no register of the machine; it prints a conversion of a
 * double as the characters it was written with, which keeps the text of a
 * typo visible instead of reading an argument that is not there.
 */
#include "ulibc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ULIBC_WITH_FLOAT
#include "format_float.h"
#endif

#include "file_private.h"

#define FORMAT_BUFFER 128

struct output {
    int fd;
    int to_buffer;
    char *dest;
    size_t cap;
    int failed;
    size_t used;
    long total;
    char buffer[FORMAT_BUFFER];
};

static void out_flush(struct output *out)
{
    if (out->to_buffer) {
        return; /* a buffer of the caller has no descriptor behind it */
    }

    if (out->used > 0) {
        long wrote = write(out->fd, out->buffer, out->used);

        if (wrote < 0) {
            out->failed = 1;
        } else {
            out->total += wrote;
        }
        out->used = 0;
    }
}

static void out_char(struct output *out, char byte)
{
    if (out->to_buffer) {
        /*
         * The total is the length the text would have had, as snprintf
         * promises, so a caller can ask for the size of the buffer it needs.
         */
        if (out->dest != NULL && out->used + 1 < out->cap) {
            out->dest[out->used] = byte;
        }
        out->used++;
        out->total = (long)out->used;
        return;
    }

    if (out->used == sizeof(out->buffer)) {
        out_flush(out);
    }

    out->buffer[out->used++] = byte;
}

static void out_text(struct output *out, const char *text)
{
    while (*text != '\0') {
        out_char(out, *text);
        text++;
    }
}

/*
 * A field of `length` characters padded to `width`. The sign of a number and
 * the zeros of a `0` flag are one field: the zeros go after the sign, as the
 * C library writes a negative number of a width.
 */
static void out_field(struct output *out, const char *text, int length, int width, int left,
                      int zero)
{
    int pad = width - length;

    if (pad < 0) {
        pad = 0;
    }

    if (left) {
        out_text(out, text);
        while (pad-- > 0) {
            out_char(out, ' ');
        }
        return;
    }

    if (zero && length > 0 && (text[0] == '-' || text[0] == '+' || text[0] == ' ')) {
        out_char(out, text[0]);
        while (pad-- > 0) {
            out_char(out, '0');
        }
        out_text(out, text + 1);
        return;
    }

    if (zero) {
        while (pad-- > 0) {
            out_char(out, '0');
        }
        out_text(out, text);
        return;
    }

    while (pad-- > 0) {
        out_char(out, ' ');
    }
    out_text(out, text);
}

/*
 * An unsigned number in a base, padded to `width` characters. The sign, when
 * there is one, is part of the field: a zero flag pads between the sign and
 * the digits, a minus flag pads after them. A precision is the least number
 * of digits, and a precision of zero writes no digit for the value zero.
 */
/*
 * The next digit of a number in a base, from the least significant end. The
 * divisor is a constant of the switch and not the argument: a 64-bit division
 * by a variable asks for `__udivdi3` of libgcc, which a program of the board
 * (linked with -nostdlib) does not carry, while a division by ten or a shift
 * is arithmetic of the compiler itself.
 */
static unsigned next_digit(unsigned long long *value, unsigned base)
{
    unsigned digit;

    switch (base) {
    case 10:
        digit = (unsigned)(*value % 10ull);
        *value /= 10ull;
        break;
    case 8:
        digit = (unsigned)(*value & 7ull);
        *value >>= 3;
        break;
    default:
        digit = (unsigned)(*value & 15ull);
        *value >>= 4;
        break;
    }

    return digit;
}

static void out_number(struct output *out, unsigned long long value, unsigned base, int upper,
                       int width, int precision, int zero, int left, char sign, const char *prefix)
{
    char digits[32];
    int count = 0;
    int prefix_length = (prefix != NULL) ? (int)strlen(prefix) : 0;
    int pad;

    if (!(value == 0 && precision == 0)) {
        if (value == 0) {
            digits[count++] = '0';
        }
        while (value > 0) {
            unsigned digit = next_digit(&value, base);

            if (digit < 10) {
                digits[count++] = (char)('0' + (int)digit);
            } else {
                digits[count++] = (char)((upper ? 'A' : 'a') + (int)(digit - 10));
            }
        }
    }

    while (precision > 0 && count < precision && count < (int)sizeof(digits)) {
        digits[count++] = '0';
    }

    pad = width - count - prefix_length - ((sign != '\0') ? 1 : 0);
    if (pad < 0) {
        pad = 0;
    }

    if (!left && !zero) {
        while (pad-- > 0) {
            out_char(out, ' ');
        }
    }

    if (sign != '\0') {
        out_char(out, sign);
    }

    if (prefix != NULL) {
        out_text(out, prefix);
    }

    if (!left && zero && precision < 0) {
        while (pad-- > 0) {
            out_char(out, '0');
        }
    }

    while (count > 0) {
        out_char(out, digits[--count]);
    }

    if (left) {
        while (pad-- > 0) {
            out_char(out, ' ');
        }
    }
}

static void out_format(struct output *out, const char *text, va_list args)
{
    while (*text != '\0') {
        int left = 0;
        int zero = 0;
        int plus = 0;
        int space = 0;
        int alternate = 0;
        int width = 0;
        int precision = -1;
        int is_long = 0;
        int is_long_long = 0;
        char conversion;

        if (*text != '%') {
            out_char(out, *text++);
            continue;
        }

        text++;

        for (;;) {
            if (*text == '-') {
                left = 1;
                text++;
            } else if (*text == '0') {
                zero = 1;
                text++;
            } else if (*text == '+') {
                plus = 1;
                text++;
            } else if (*text == ' ') {
                space = 1;
                text++;
            } else if (*text == '#') {
                alternate = 1;
                text++;
            } else {
                break;
            }
        }

        if (*text == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            text++;
        } else {
            while (*text >= '0' && *text <= '9') {
                width = width * 10 + (*text - '0');
                text++;
            }
        }

        if (*text == '.') {
            text++;
            precision = 0;
            if (*text == '*') {
                precision = va_arg(args, int);
                if (precision < 0) {
                    precision = -1;
                }
                text++;
            } else {
                while (*text >= '0' && *text <= '9') {
                    precision = precision * 10 + (*text - '0');
                    text++;
                }
            }
        }

        /* The precision: the bytes of a string that are printed. The
         * numbers of this formatter do not use it, and a number with one is
         * printed as the number itself. */
        if (*text == '.') {
            text++;
            precision = 0;
            while (*text >= '0' && *text <= '9') {
                precision = precision * 10 + (*text - '0');
                text++;
            }
        }

        if (*text == 'l') {
            is_long = 1;
            text++;
            if (*text == 'l') {
                is_long_long = 1;
                text++;
            }
        } else if (*text == 'z' || *text == 't') {
            is_long = 1;
            text++;
        } else if (*text == 'j') {
            is_long_long = 1;
            text++;
        } else if (*text == 'h') {
            /* A short is promoted to an int in the arguments of a variadic
             * call; the length is read and has no other effect. */
            text++;
            if (*text == 'h') {
                text++;
            }
        }

        conversion = *text;
        if (conversion == '\0') {
            break; /* a format that ends in the middle of a conversion */
        }
        text++;

        switch (conversion) {
        case '%':
            out_char(out, '%');
            break;
        case 'c': {
            char single = (char)va_arg(args, int);

            out_field(out, &single, 1, width, left, 0);
            break;
        }
        case 's': {
            const char *word = va_arg(args, const char *);
            int length;

            if (word == NULL) {
                word = "(null)";
            }
            length = (int)strlen(word);
            if (precision >= 0 && length > precision) {
                length = precision;
            }
            if (!left) {
                for (int pad = width - length; pad > 0; pad--) {
                    out_char(out, ' ');
                }
            }
            for (int i = 0; i < length; i++) {
                out_char(out, word[i]);
            }
            if (left) {
                for (int pad = width - length; pad > 0; pad--) {
                    out_char(out, ' ');
                }
            }
            break;
        }
        case 'd':
        case 'i': {
            long long value;

            if (is_long_long) {
                value = va_arg(args, long long);
            } else if (is_long) {
                value = va_arg(args, long);
            } else {
                value = va_arg(args, int);
            }

            unsigned long long magnitude;
            char sign = '\0';

            if (value < 0) {
                sign = '-';
                /* The most negative number has no positive counterpart in the
                 * same width; taking the magnitude this way is what every
                 * formatter does and it is not undefined. */
                magnitude = 0ull - (unsigned long long)value;
            } else {
                magnitude = (unsigned long long)value;
                if (plus) {
                    sign = '+';
                } else if (space) {
                    sign = ' ';
                }
            }
            out_number(out, magnitude, 10, 0, width, precision, zero, left, sign, NULL);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            unsigned long long value;

            if (is_long_long) {
                value = va_arg(args, unsigned long long);
            } else if (is_long) {
                value = va_arg(args, unsigned long);
            } else {
                value = va_arg(args, unsigned int);
            }

            unsigned base = (conversion == 'o') ? 8u : ((conversion == 'u') ? 10u : 16u);
            const char *prefix = NULL;

            if (alternate) {
                if (conversion == 'o') {
                    prefix = "0";
                } else if (conversion == 'x' && value != 0) {
                    prefix = "0x";
                } else if (conversion == 'X' && value != 0) {
                    prefix = "0X";
                }
            }

            out_number(out, value, base, conversion == 'X', width, precision, zero, left, '\0',
                       prefix);
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(args, void *);

            out_number(out, (unsigned long long)value, 16, 0, width, -1, zero, left, '\0', "0x");
            break;
        }
#ifdef ULIBC_WITH_FLOAT
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            char number[512];
            double value = va_arg(args, double);
            int length = ulibc_double_text(number, sizeof(number), value, conversion, precision,
                                           plus, space, alternate);

            out_field(out, number, length, width, left, zero);
            break;
        }
#endif
        default:
            /* An unknown conversion is printed as it was written, so that a
             * typo is visible instead of silently swallowing the argument. */
            out_char(out, '%');
            out_char(out, conversion);
            break;
        }
    }
}

int vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
    struct output out;
    long total;

    out.fd = -1;
    out.to_buffer = 1;
    out.dest = buffer;
    out.cap = size;
    out.failed = 0;
    out.used = 0;
    out.total = 0;

    out_format(&out, format, args);

    total = out.total;

    if (buffer != NULL && size > 0) {
        size_t end = (out.used < size) ? out.used : size - 1u;

        buffer[end] = '\0';
    }

    return (int)total;
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buffer, size, format, args);
    va_end(args);

    return length;
}

int sprintf(char *buffer, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vsnprintf(buffer, (size_t)-1, format, args);
    va_end(args);

    return length;
}

static int out_to_descriptor(int fd, const char *format, va_list args)
{
    struct output out;

    out.fd = fd;
    out.to_buffer = 0;
    out.dest = NULL;
    out.cap = 0;
    out.failed = 0;
    out.used = 0;
    out.total = 0;

    out_format(&out, format, args);
    out_flush(&out);

    return out.failed ? -1 : (int)out.total;
}

int vfprintf(FILE *stream, const char *format, va_list args)
{
    if (stream == NULL) {
        return -1;
    }

    return out_to_descriptor(stream->fd, format, args);
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = vfprintf(stream, format, args);
    va_end(args);

    return length;
}

int printf(const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = out_to_descriptor(ULIBC_STDOUT, format, args);
    va_end(args);

    return length;
}

int vprintf(const char *format, va_list args)
{
    return out_to_descriptor(ULIBC_STDOUT, format, args);
}

int putchar(int byte)
{
    char single = (char)byte;

    return (write(ULIBC_STDOUT, &single, 1) == 1) ? (int)(unsigned char)single : -1;
}

int puts(const char *text)
{
    long wrote = write(ULIBC_STDOUT, text, strlen(text));

    if (wrote < 0) {
        return (int)wrote;
    }

    wrote = write(ULIBC_STDOUT, "\n", 1);

    return (wrote < 0) ? (int)wrote : 0;
}
