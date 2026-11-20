/*
 * Micro OS — the strings and the formatting the kernel needs, without a libc.
 *
 * The kernel is compiled freestanding: it may use <string.h> and <stdio.h>
 * for the declarations, but the definitions have to come from somewhere, and
 * on this machine there is no libc under the kernel. This file is the whole
 * of what the kernel actually calls: the string functions of the C library
 * and a small formatter for the log.
 *
 * The formatter speaks the conversions the kernel writes and no more: %s
 * (with a left justified width), %c, %d, %u, %x and %p. Anything else is
 * printed literally, which is a bug of the caller and visible at once.
 */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "kernel/types.h"

size_t strlen(const char *text)
{
    size_t len = 0;

    while (text[len] != '\0') {
        len++;
    }

    return len;
}

char *strncpy(char *dst, const char *src, size_t size)
{
    size_t i = 0;

    while (i < size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }

    while (i < size) {
        dst[i++] = '\0';
    }

    return dst;
}

int strcmp(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        left++;
        right++;
    }

    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        if (left[i] != right[i] || left[i] == '\0') {
            return (int)(unsigned char)left[i] - (int)(unsigned char)right[i];
        }
    }

    return 0;
}

/* --- the formatter --- */

struct sink {
    char *buf;
    size_t size;
    size_t written; /* what the whole answer would have taken */
};

static void sink_char(struct sink *s, char c)
{
    if (s->written + 1 < s->size) {
        s->buf[s->written] = c;
    }

    s->written++;
}

static void sink_text(struct sink *s, const char *text)
{
    while (*text != '\0') {
        sink_char(s, *text++);
    }
}

static void sink_unsigned(struct sink *s, unsigned long long value, unsigned base, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;

    if (value == 0) {
        sink_char(s, '0');
        return;
    }

    while (value != 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = digits[value % base];
        value /= base;
    }

    while (n > 0) {
        sink_char(s, tmp[--n]);
    }
}

/*
 * The conversions the kernel writes: %s, %c, %d, %u, %x, %X, %p and %%.
 * The flags are the ones a person uses in a table: "-" (to the left), "0"
 * (pad a number with zeroes) and a width, given as digits or as "*" (the
 * width is the next argument); a precision "." cuts a string or pads a
 * number, and it may be "*" as well. What is not here (%f, %e, %g) needs a
 * floating point unit the kernel does not save, and is printed literally.
 */
static int format(struct sink *s, const char *fmt, va_list ap)
{
    while (*fmt != '\0') {
        bool left = false;
        bool zero = false;
        int width = 0;
        int precision = -1;

        if (*fmt != '%') {
            sink_char(s, *fmt++);
            continue;
        }

        fmt++;

        if (*fmt == '-') {
            left = true;
            fmt++;
        }

        if (*fmt == '0') {
            zero = true;
            fmt++;
        }

        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        if (*fmt == '.') {
            fmt++;

            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                fmt++;
            }
        }

        switch (*fmt) {
        case 's': {
            const char *text = va_arg(ap, const char *);
            int len;

            if (text == NULL) {
                text = "(null)";
            }

            len = (int)strlen(text);

            if (precision >= 0 && len > precision) {
                len = precision;
            }

            if (!left) {
                for (int i = len; i < width; i++) {
                    sink_char(s, ' ');
                }
            }

            for (int i = 0; i < len; i++) {
                sink_char(s, text[i]);
            }

            if (left) {
                for (int i = len; i < width; i++) {
                    sink_char(s, ' ');
                }
            }
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);

            if (!left) {
                for (int i = 1; i < width; i++) {
                    sink_char(s, ' ');
                }
            }

            sink_char(s, c);

            if (left) {
                for (int i = 1; i < width; i++) {
                    sink_char(s, ' ');
                }
            }
            break;
        }
        case 'd':
        case 'u':
        case 'x':
        case 'X': {
            char number[24];
            struct sink scratch = {.buf = number, .size = sizeof(number), .written = 0};
            bool negative = false;
            int len;
            int pad;

            if (*fmt == 'd') {
                int value = va_arg(ap, int);

                if (value < 0) {
                    negative = true;
                    value = (int)(-(long long)value);
                }
                sink_unsigned(&scratch, (unsigned long long)value, 10, false);
            } else if (*fmt == 'u') {
                sink_unsigned(&scratch, va_arg(ap, unsigned int), 10, false);
            } else {
                sink_unsigned(&scratch, va_arg(ap, unsigned int), 16, *fmt == 'X');
            }

            len = (int)scratch.written;

            /* A precision on a number is the least number of digits. */
            if (precision > len) {
                for (int i = len; i < precision; i++) {
                    sink_char(&scratch, '0');
                }
                len = precision;
            }

            pad = width - len - (negative ? 1 : 0);

            if (!left) {
                for (int i = 0; i < pad; i++) {
                    sink_char(s, zero ? '0' : ' ');
                }
            }

            if (negative) {
                sink_char(s, '-');
            }

            for (int i = 0; i < len; i++) {
                sink_char(s, number[i]);
            }

            if (left) {
                for (int i = 0; i < pad; i++) {
                    sink_char(s, ' ');
                }
            }
            break;
        }
        case 'p':
            sink_text(s, "0x");
            sink_unsigned(s, (unsigned long long)(uintptr_t)va_arg(ap, void *), 16, false);
            break;
        case '%':
            sink_char(s, '%');
            break;
        case '\0':
            /* A format that ends in a single % is printed as it is. */
            sink_char(s, '%');
            return (int)s->written;
        default:
            sink_char(s, '%');
            sink_char(s, *fmt);
            break;
        }

        fmt++;
    }

    return (int)s->written;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s = {.buf = buf, .size = size, .written = 0};

    if (buf != NULL && size > 0) {
        int rc = format(&s, fmt, ap);

        buf[s.written < size ? s.written : size - 1] = '\0';
        return rc;
    }

    return format(&s, fmt, ap);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int rc;

    va_start(ap, fmt);
    rc = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    return rc;
}

char *strchr(const char *text, int c)
{
    for (;; text++) {
        if (*text == (char)c) {
            return (char *)text;
        }

        if (*text == '\0') {
            return NULL;
        }
    }
}

/*
 * The last place of a character in a string. MicroFS uses it to cut a path
 * into the directory and the name, which is the one thing of the C library
 * the filesystem needs beyond the copies and the lengths above.
 */
char *strrchr(const char *text, int c)
{
    const char *last = NULL;

    for (;; text++) {
        if (*text == (char)c) {
            last = text;
        }

        if (*text == '\0') {
            /* The terminator is a place the character may be found at, as the
             * C library says; a string without the character has none. */
            return (char *)last;
        }
    }
}
