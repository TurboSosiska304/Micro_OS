/*
 * Micro OS — the formatter and the console of a program.
 *
 * The output goes through write(1, ...) in pieces of a fixed buffer: a
 * program has no allocator of its own, and a formatter that asked the kernel
 * for memory would make every printf a call. The buffer is small because the
 * stack of a program is 64 KiB and a line of this system is short.
 *
 * The conversions are the ones a program of the tree uses - %d, %i, %u, %x,
 * %X, %c, %s, %p and %% - with a width and the flags '-' and '0'. There is no
 * precision and no '*' width: they are the part of printf that costs the most
 * and that no program of this system asks for.
 */
#include "ulibc.h"

#include <stdarg.h>
#include <stdint.h>

#define FORMAT_BUFFER 128

struct output {
    int fd;
    int failed;
    size_t used;
    long total;
    char buffer[FORMAT_BUFFER];
};

static void out_flush(struct output *out)
{
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
 * An unsigned number in a base, padded to `width` characters. The sign, when
 * there is one, is part of the field: a zero flag pads between the sign and
 * the digits, a minus flag pads after them.
 */
static void out_number(struct output *out, unsigned long value, unsigned base, int upper, int width,
                       int zero, int left, char sign)
{
    char digits[32];
    int count = 0;
    int pad;
    int used = (sign != '\0') ? 1 : 0;

    if (value == 0) {
        digits[count++] = '0';
    }

    while (value > 0) {
        unsigned long digit = value % base;

        if (digit < 10) {
            digits[count++] = (char)('0' + (int)digit);
        } else {
            digits[count++] = (char)((upper ? 'A' : 'a') + (int)(digit - 10));
        }
        value /= base;
    }

    pad = width - count - used;
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

    if (!left && zero) {
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
        int width = 0;
        int is_long = 0;
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
            } else {
                break;
            }
        }

        while (*text >= '0' && *text <= '9') {
            width = width * 10 + (*text - '0');
            text++;
        }

        if (*text == 'l' || *text == 'z') {
            is_long = 1;
            text++;
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
        case 'c':
            out_char(out, (char)va_arg(args, int));
            break;
        case 's': {
            const char *word = va_arg(args, const char *);
            size_t length;

            if (word == NULL) {
                word = "(null)";
            }
            length = strlen(word);
            if (!left) {
                for (int pad = width - (int)length; pad > 0; pad--) {
                    out_char(out, ' ');
                }
            }
            out_text(out, word);
            if (left) {
                for (int pad = width - (int)length; pad > 0; pad--) {
                    out_char(out, ' ');
                }
            }
            break;
        }
        case 'd':
        case 'i': {
            long value = is_long ? va_arg(args, long) : (long)va_arg(args, int);
            unsigned long magnitude;
            char sign = '\0';

            if (value < 0) {
                sign = '-';
                /* The most negative number has no positive counterpart in the
                 * same width; taking the magnitude this way is what every
                 * formatter does and it is not undefined. */
                magnitude = (unsigned long)0 - (unsigned long)value;
            } else {
                magnitude = (unsigned long)value;
            }
            out_number(out, magnitude, 10, 0, width, zero, left, sign);
            break;
        }
        case 'u': {
            unsigned long value =
                is_long ? va_arg(args, unsigned long) : (unsigned long)va_arg(args, unsigned int);
            out_number(out, value, 10, 0, width, zero, left, '\0');
            break;
        }
        case 'x':
        case 'X': {
            unsigned long value =
                is_long ? va_arg(args, unsigned long) : (unsigned long)va_arg(args, unsigned int);
            out_number(out, value, 16, conversion == 'X', width, zero, left, '\0');
            break;
        }
        case 'p': {
            uintptr_t value = (uintptr_t)va_arg(args, void *);

            out_text(out, "0x");
            out_number(out, (unsigned long)value, 16, 0, 0, 0, 0, '\0');
            break;
        }
        default:
            /* An unknown conversion is printed as it was written, so that a
             * typo is visible instead of silently swallowing the argument. */
            out_char(out, '%');
            out_char(out, conversion);
            break;
        }
    }
}

int printf(const char *format, ...)
{
    struct output out;
    va_list args;

    out.fd = ULIBC_STDOUT;
    out.failed = 0;
    out.used = 0;
    out.total = 0;

    va_start(args, format);
    out_format(&out, format, args);
    va_end(args);

    out_flush(&out);

    return out.failed ? -1 : (int)out.total;
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
