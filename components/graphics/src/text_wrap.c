/*
 * Micro OS — one line of text cut into the rows of a picture.
 *
 * See text_wrap.h for why this file exists on its own.
 */
#include "graphics/text_wrap.h"

#include <string.h>

int text_wrap_take(char *dst, int dst_max, const char *line, int columns, int row)
{
    int len;
    int offset;
    int n;

    if (dst == NULL || dst_max <= 0 || line == NULL || columns < 1 || row < 0) {
        return 0;
    }

    len = (int)strlen(line);
    offset = row * columns;

    if (offset >= len) {
        dst[0] = '\0';
        return 0;
    }

    n = len - offset;
    if (n > columns) {
        n = columns;
    }
    if (n > dst_max - 1) {
        n = dst_max - 1;
    }

    memcpy(dst, line + offset, (size_t)n);
    dst[n] = '\0';

    return n;
}
