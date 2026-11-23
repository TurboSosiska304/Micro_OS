/*
 * Micro OS — the numbers and the words of a line of text, read by a format.
 *
 * The shell of SQLite reads the answers of its own `dbinfo` and the arguments
 * of `.mode` with `sscanf`; a ported source that walks a file of records asks
 * the same of `fscanf` and `scanf`. The two readers are one: a source of bytes
 * is either the memory of a string or a stream of the library, and the scanner
 * walks the format over it once. That is why `fscanf` does not need the block
 * of the stream to itself - it reads the bytes through `fgetc` and keeps the
 * one byte of look-ahead a conversion needs in a stack of its own.
 *
 * The conversions are the ones of such a line: %d %i %u %o %x %X %p %s %c %n
 * %%, the width of a field, the `*` that drops an answer, and - in a build
 * that defines `ULIBC_WITH_FLOAT` - %f %e %g and their capitals. A literal has
 * to match, whitespace of the format matches any run of whitespace, and the
 * first mismatch ends the scan, as C promises. A conversion of a number reads
 * the longest text the conversion accepts and gives the rest of it back to the
 * source, so a reader of "12abc" with "%d%s" leaves "abc" for the second
 * conversion.
 *
 * The answer is the number of the conversions that stored a value, `%n` not
 * counted; `EOF` when the input failed before any conversion completed (the
 * end of an empty string or of a file), and 0 when the first thing the format
 * asked for was not there. That is the rule of the C library, and the one a
 * loop `while (fscanf(...) == 2)` is written on.
 */
#include "ulibc.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The look-ahead of a scan. A conversion reads one byte more than the text it
 * takes - the byte that ended the field - and the tail of a number that
 * `strtoul` did not use is given back the same way. The patterns of the
 * library are short, so a handful of bytes is the whole of the state.
 */
#define SCAN_PUSHBACK 8

struct scan_source {
    const char *text; /* the string of `sscanf`, or NULL for a stream */
    const char *at;
    FILE *stream;
    int pushed[SCAN_PUSHBACK];
    int npushed;
    long offset; /* the bytes consumed, for %n */
};

static int space_byte(int byte)
{
    return byte == ' ' || (byte >= '\t' && byte <= '\r');
}

static int set_has(const char *set, int byte)
{
    if (byte == '\0') {
        return 0;
    }

    return strchr(set, byte) != NULL;
}

static int source_get(struct scan_source *source)
{
    int byte;

    if (source->npushed > 0) {
        byte = source->pushed[--source->npushed];
    } else if (source->text != NULL) {
        if (*source->at == '\0') {
            return EOF;
        }
        byte = (unsigned char)*source->at++;
    } else {
        byte = fgetc(source->stream);
        if (byte == EOF) {
            return EOF;
        }
    }

    source->offset++;

    return byte;
}

static void source_unget(struct scan_source *source, int byte)
{
    if (byte == EOF || source->npushed >= SCAN_PUSHBACK) {
        return;
    }

    source->pushed[source->npushed++] = byte;
    source->offset--;
}

static void source_skip_space(struct scan_source *source)
{
    int byte;

    do {
        byte = source_get(source);
    } while (byte != EOF && space_byte(byte));

    source_unget(source, byte);
}

/*
 * Gather the bytes of a field. The run ends at the first byte that is not in
 * `set` (given back to the source) or at the end of the input. The answer is
 * the count; `at_end` says whether the source was at its end before a byte was
 * read, which is the difference between the end of the input and a mismatch.
 */
static int field_read(struct scan_source *source, char *field, size_t limit, int width,
                      const char *set, int *at_end)
{
    int count = 0;
    int byte;

    *at_end = 0;

    while (width <= 0 || count < width) {
        if ((size_t)count + 1 >= limit) {
            break;
        }

        byte = source_get(source);
        if (byte == EOF) {
            *at_end = (count == 0);
            break;
        }
        if (!set_has(set, byte)) {
            source_unget(source, byte);
            break;
        }

        field[count++] = (char)byte;
    }

    field[count] = '\0';

    return count;
}

/* Give back the part of the field the conversion of the number did not use. */
static void field_unuse(struct scan_source *source, const char *field, int count, int used)
{
    int at;

    for (at = count; at > used; at--) {
        source_unget(source, (unsigned char)field[at - 1]);
    }
}

static const char *number_set(int base)
{
    switch (base) {
    case 8:
        return "01234567+-";
    case 16:
        return "0123456789abcdefABCDEFxX+-";
    case 0:
        return "0123456789abcdefABCDEFxX+-";
    default:
        return "0123456789+-";
    }
}

static int scan_core(struct scan_source *source, const char *format, va_list args,
                     int *input_failure, int *any_done)
{
    int matched = 0;
    int byte;

    *input_failure = 0;
    *any_done = 0;

    while (*format != '\0') {
        int suppress = 0;
        int width = 0;
        int modify_long = 0;
        int modify_llong = 0;
        int is_short = 0;
        int is_char = 0;
        int h_count = 0;
        int l_count = 0;
        char conversion;

        if (space_byte((unsigned char)*format)) {
            source_skip_space(source);
            while (space_byte((unsigned char)*format)) {
                format++;
            }
            continue;
        }

        if (*format != '%') {
            byte = source_get(source);
            if (byte == EOF) {
                if (!*any_done) {
                    *input_failure = 1;
                }
                break;
            }
            if (byte != (unsigned char)*format) {
                source_unget(source, byte);
                break;
            }
            format++;
            continue;
        }

        format++;
        if (*format == '%') {
            byte = source_get(source);
            if (byte == EOF) {
                if (!*any_done) {
                    *input_failure = 1;
                }
                break;
            }
            if (byte != '%') {
                source_unget(source, byte);
                break;
            }
            format++;
            *any_done = 1;
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

        while (*format == 'h' || *format == 'l' || *format == 'z' || *format == 't' ||
               *format == 'j') {
            if (*format == 'h') {
                h_count++;
            } else if (*format == 'l') {
                l_count++;
            } else {
                /* `z`, `t` and `j` are the width of a long on both machines
                 * of the system: a size_t, a ptrdiff_t and an intmax_t are
                 * four bytes on Xtensa and eight on x86-64, as a long is. */
                modify_long = 1;
            }
            format++;
        }
        if (l_count >= 2) {
            modify_llong = 1;
        } else if (l_count == 1) {
            modify_long = 1;
        }
        if (h_count >= 2) {
            is_char = 1;
        } else if (h_count == 1) {
            is_short = 1;
        }

        conversion = *format;
        if (conversion == '\0') {
            break;
        }
        format++;

        if (conversion == 'n') {
            if (!suppress) {
                *(va_arg(args, int *)) = (int)source->offset;
            }
            *any_done = 1;
            continue;
        }

        if (conversion == 'c') {
            int count = (width > 0) ? width : 1;
            char *out = NULL;
            int got = 0;

            if (!suppress) {
                out = va_arg(args, char *);
            }
            while (got < count) {
                byte = source_get(source);
                if (byte == EOF) {
                    break;
                }
                if (!suppress) {
                    out[got] = (char)byte;
                }
                got++;
            }
            if (got == 0) {
                if (!*any_done) {
                    *input_failure = 1;
                }
                break;
            }
            if (got < count) {
                break;
            }
            if (!suppress) {
                matched++;
            }
            *any_done = 1;
            continue;
        }

        if (conversion == 's') {
            char *out = NULL;
            int used = 0;

            source_skip_space(source);
            byte = source_get(source);
            if (byte == EOF) {
                if (!*any_done) {
                    *input_failure = 1;
                }
                break;
            }
            source_unget(source, byte);

            if (!suppress) {
                out = va_arg(args, char *);
            }
            while (width == 0 || used < width) {
                byte = source_get(source);
                if (byte == EOF) {
                    break;
                }
                if (space_byte(byte)) {
                    source_unget(source, byte);
                    break;
                }
                if (!suppress) {
                    out[used] = (char)byte;
                }
                used++;
            }
            if (used == 0) {
                break;
            }
            if (!suppress) {
                out[used] = '\0';
                matched++;
            }
            *any_done = 1;
            continue;
        }

        if (conversion == 'd' || conversion == 'i' || conversion == 'u' || conversion == 'o' ||
            conversion == 'x' || conversion == 'X' || conversion == 'p') {
            char field[64];
            int base = 10;
            int at_end = 0;
            int count;
            int used;
            char *end = NULL;

            if (conversion == 'i') {
                base = 0;
            } else if (conversion == 'o') {
                base = 8;
            } else if (conversion == 'x' || conversion == 'X' || conversion == 'p') {
                base = 16;
            }

            source_skip_space(source);
            count = field_read(source, field, sizeof(field), width, number_set(base), &at_end);
            if (count == 0) {
                if (at_end && !*any_done) {
                    *input_failure = 1;
                }
                break;
            }

            if (conversion == 'd' || conversion == 'i') {
                long long value = (long long)strtoull(field, &end, base);

                used = (int)(end - field);
                if (used == 0) {
                    field_unuse(source, field, count, 0);
                    break;
                }
                field_unuse(source, field, count, used);
                if (!suppress) {
                    if (is_char) {
                        *(va_arg(args, signed char *)) = (signed char)value;
                    } else if (is_short) {
                        *(va_arg(args, short *)) = (short)value;
                    } else if (modify_llong) {
                        *(va_arg(args, long long *)) = value;
                    } else if (modify_long) {
                        *(va_arg(args, long *)) = (long)value;
                    } else {
                        *(va_arg(args, int *)) = (int)value;
                    }
                    matched++;
                }
            } else {
                unsigned long long value = strtoull(field, &end, base);

                used = (int)(end - field);
                if (used == 0) {
                    field_unuse(source, field, count, 0);
                    break;
                }
                field_unuse(source, field, count, used);
                if (!suppress) {
                    if (conversion == 'p') {
                        *(va_arg(args, void **)) = (void *)(uintptr_t)value;
                    } else if (is_char) {
                        *(va_arg(args, unsigned char *)) = (unsigned char)value;
                    } else if (is_short) {
                        *(va_arg(args, unsigned short *)) = (unsigned short)value;
                    } else if (modify_llong) {
                        *(va_arg(args, unsigned long long *)) = value;
                    } else if (modify_long) {
                        *(va_arg(args, unsigned long *)) = (unsigned long)value;
                    } else {
                        *(va_arg(args, unsigned int *)) = (unsigned int)value;
                    }
                    matched++;
                }
            }
            *any_done = 1;
            continue;
        }

#ifdef ULIBC_WITH_FLOAT
        if (conversion == 'f' || conversion == 'e' || conversion == 'g' || conversion == 'F' ||
            conversion == 'E' || conversion == 'G') {
            char field[128];
            int at_end = 0;
            int count;
            int used;
            char *end = NULL;
            double value;

            source_skip_space(source);
            count = field_read(source, field, sizeof(field), width, "+-0123456789.eEinfatyINFATY",
                               &at_end);
            if (count == 0) {
                if (at_end && !*any_done) {
                    *input_failure = 1;
                }
                break;
            }

            value = strtod(field, &end);
            used = (int)(end - field);
            if (used == 0) {
                field_unuse(source, field, count, 0);
                break;
            }
            field_unuse(source, field, count, used);
            if (!suppress) {
                *(va_arg(args, double *)) = value;
                matched++;
            }
            *any_done = 1;
            continue;
        }
#endif /* ULIBC_WITH_FLOAT */

        break; /* a conversion this reader does not know */
    }

    return matched;
}

int vsscanf(const char *text, const char *format, va_list args)
{
    struct scan_source source;
    int input_failure;
    int any_done;
    int matched;

    if (text == NULL || format == NULL) {
        return EOF;
    }

    source.text = text;
    source.at = text;
    source.stream = NULL;
    source.npushed = 0;
    source.offset = 0;

    matched = scan_core(&source, format, args, &input_failure, &any_done);

    return (!any_done && input_failure) ? EOF : matched;
}

int sscanf(const char *text, const char *format, ...)
{
    va_list args;
    int answer;

    if (text == NULL || format == NULL) {
        return EOF;
    }

    va_start(args, format);
    answer = vsscanf(text, format, args);
    va_end(args);

    return answer;
}

int vfscanf(FILE *stream, const char *format, va_list args)
{
    struct scan_source source;
    int input_failure;
    int any_done;
    int matched;

    if (stream == NULL || format == NULL) {
        return EOF;
    }

    source.text = NULL;
    source.at = NULL;
    source.stream = stream;
    source.npushed = 0;
    source.offset = 0;

    matched = scan_core(&source, format, args, &input_failure, &any_done);

    return (!any_done && input_failure) ? EOF : matched;
}

int fscanf(FILE *stream, const char *format, ...)
{
    va_list args;
    int answer;

    if (stream == NULL || format == NULL) {
        return EOF;
    }

    va_start(args, format);
    answer = vfscanf(stream, format, args);
    va_end(args);

    return answer;
}

int vscanf(const char *format, va_list args)
{
    if (format == NULL) {
        return EOF;
    }

    return vfscanf(stdin, format, args);
}

int scanf(const char *format, ...)
{
    va_list args;
    int answer;

    if (format == NULL) {
        return EOF;
    }

    va_start(args, format);
    answer = vfscanf(stdin, format, args);
    va_end(args);

    return answer;
}
