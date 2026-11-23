/*
 * Micro OS — the allocation of a formatted string.
 *
 * `asprintf` is the formatter of `src/stdio.c` and the arena of the program:
 * the first pass asks `vsnprintf` for the length of the text, the block is
 * allocated, and the second pass writes it. A library that builds a path or a
 * sentence of its own (libmagic names each file of the magic database this
 * way) is the caller.
 *
 * The answer is the length of the text, or -1 with `ENOMEM` when the arena is
 * full; a caller frees the block with `free()`, as in the library of a
 * Unix-like system.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int format_alloc(char **out, const char *format, va_list args)
{
    va_list again;
    char *text;
    int length;

    if (out == NULL || format == NULL) {
        errno = EINVAL;
        return -1;
    }

    va_copy(again, args);
    length = vsnprintf(NULL, 0, format, again);
    va_end(again);
    if (length < 0) {
        return -1;
    }

    text = malloc((size_t)length + 1u);
    if (text == NULL) {
        errno = ENOMEM;
        return -1;
    }

    (void)vsnprintf(text, (size_t)length + 1u, format, args);
    *out = text;
    return length;
}

int vasprintf(char **out, const char *format, va_list args)
{
    return format_alloc(out, format, args);
}

int asprintf(char **out, const char *format, ...)
{
    va_list args;
    int length;

    va_start(args, format);
    length = format_alloc(out, format, args);
    va_end(args);
    return length;
}
