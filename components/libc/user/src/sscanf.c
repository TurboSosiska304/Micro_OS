/*
 * Micro OS — the numbers and the words of a line of text.
 *
 * The shell of SQLite reads the answers of its own `dbinfo` and the
 * arguments of `.mode` with `sscanf`; the library of a program had no such
 * function because no program of the tree read a structured line before.
 * The conversions are the ones of such a line: %d %i %u %o %x %s %c %n, the
 * width of a field, the `*` that drops the answer, and the floats through
 * strtod. A literal has to match, whitespace of the format matches any run
 * of whitespace, and the first mismatch ends the scan, as C promises.
 */
#include "ulibc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int space(int byte)
{
    return byte == ' ' || (byte >= '\t' && byte <= '\r');
}

static void skip_space(const char **cursor)
{
    while (space((unsigned char)**cursor)) {
        (*cursor)++;
    }
}

int sscanf(const char *text, const char *format, ...)
{
    const char *in = text;
    va_list args;
    int matched = 0;

    va_start(args, format);

    while (*format != '\0') {
        int suppress = 0;
        int width = 0;
        int is_long = 0;
        int is_short = 0;
        char conversion;

        if (space((unsigned char)*format)) {
            skip_space(&in);
            while (space((unsigned char)*format)) {
                format++;
            }
            continue;
        }

        if (*format != '%') {
            if (*in != *format) {
                break;
            }
            in++;
            format++;
            continue;
        }

        format++;
        if (*format == '%') {
            if (*in != '%') {
                break;
            }
            in++;
            format++;
            continue;
        }

        if (*format == '*') {
            suppress = 1;
            format++;
        }

        while (*format >= '0' && *format <= '9') {
            width = width * 10 + (*format - '0');
            format++;
        }

        while (*format == 'l' || *format == 'z' || *format == 't' || *format == 'j' ||
               *format == 'h') {
            if (*format == 'l' || *format == 'z' || *format == 't' || *format == 'j') {
                is_long = 1;
            } else {
                is_short = 1;
            }
            format++;
        }

        conversion = *format;
        if (conversion == '\0') {
            break;
        }
        format++;

        if (conversion == 'n') {
            if (!suppress) {
                *(va_arg(args, int *)) = (int)(in - text);
            }
            continue;
        }

        if (conversion == 'c') {
            int count = (width > 0) ? width : 1;
            char *out;

            if (*in == '\0') {
                break;
            }
            if (!suppress) {
                out = va_arg(args, char *);
            }
            while (count-- > 0 && *in != '\0') {
                if (!suppress) {
                    *out++ = *in;
                }
                in++;
            }
            if (!suppress) {
                matched++;
            }
            continue;
        }

        if (conversion == 's') {
            char *out = NULL;
            int used = 0;

            skip_space(&in);
            if (*in == '\0') {
                break;
            }
            if (!suppress) {
                out = va_arg(args, char *);
            }
            while (*in != '\0' && !space((unsigned char)*in) && (width == 0 || used < width)) {
                if (!suppress) {
                    *out++ = *in;
                }
                in++;
                used++;
            }
            if (!suppress) {
                *out = '\0';
                matched++;
            }
            continue;
        }

        if (conversion == 'd' || conversion == 'i' || conversion == 'u' || conversion == 'o' ||
            conversion == 'x' || conversion == 'X' || conversion == 'p') {
            char field[64];
            int base = 10;
            int used = 0;
            unsigned long value;
            char *end = NULL;

            skip_space(&in);
            if (*in == '\0') {
                break;
            }
            if (conversion == 'i') {
                base = 0;
            } else if (conversion == 'o') {
                base = 8;
            } else if (conversion == 'x' || conversion == 'X' || conversion == 'p') {
                base = 16;
            }
            if (width > 0) {
                while (*in != '\0' && used < width && used < (int)sizeof(field) - 1) {
                    field[used++] = *in++;
                }
                field[used] = '\0';
            } else {
                used = (int)strlen(in);
                if (used >= (int)sizeof(field)) {
                    used = (int)sizeof(field) - 1;
                }
                memcpy(field, in, (size_t)used);
                field[used] = '\0';
            }

            value = strtoul(field, &end, base);
            if (end == field) {
                break; /* not a number: the scan ends here */
            }
            in += (end - field);

            if (!suppress) {
                if (conversion == 'd' || conversion == 'i') {
                    if (is_long) {
                        *(va_arg(args, long *)) = (long)value;
                    } else if (is_short) {
                        *(va_arg(args, short *)) = (short)value;
                    } else {
                        *(va_arg(args, int *)) = (int)value;
                    }
                } else {
                    if (is_long) {
                        *(va_arg(args, unsigned long *)) = value;
                    } else if (is_short) {
                        *(va_arg(args, unsigned short *)) = (unsigned short)value;
                    } else {
                        *(va_arg(args, unsigned int *)) = (unsigned int)value;
                    }
                }
                matched++;
            }
            continue;
        }

        if (conversion == 'f' || conversion == 'e' || conversion == 'g' || conversion == 'E' ||
            conversion == 'G') {
            char field[128];
            int used = 0;
            char *end = NULL;
            double value;

            skip_space(&in);
            if (*in == '\0') {
                break;
            }
            if (width > 0) {
                while (*in != '\0' && used < width && used < (int)sizeof(field) - 1) {
                    field[used++] = *in++;
                }
                field[used] = '\0';
            } else {
                used = (int)strlen(in);
                if (used >= (int)sizeof(field)) {
                    used = (int)sizeof(field) - 1;
                }
                memcpy(field, in, (size_t)used);
                field[used] = '\0';
            }
            value = strtod(field, &end);
            if (end == field) {
                break;
            }
            in += (end - field);
            if (!suppress) {
                *(va_arg(args, double *)) = value;
                matched++;
            }
            continue;
        }

        break; /* a conversion this reader does not know */
    }

    va_end(args);

    return matched;
}
