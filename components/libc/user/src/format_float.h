/*
 * Micro OS — the contract of the formatter with the number with a point.
 *
 * `src/stdio.c` parses a conversion and pads a field; the digits of a double
 * come from `src/format_float.c`. The two files are one mechanism and this
 * header is the seam between them, not a part of the library a program
 * includes.
 */
#ifndef MICRO_OS_LIBC_USER_FORMAT_FLOAT_H
#define MICRO_OS_LIBC_USER_FORMAT_FLOAT_H

#include <stddef.h>

/*
 * The text of `value` as `conversion` writes it (f, F, e, E, g or G), with
 * the sign and without the padding of the width. The answer is the number of
 * characters, and the text is terminated inside `cap` bytes.
 */
int ulibc_double_text(char *out, size_t cap, double value, char conversion, int precision, int plus,
                      int space, int alternate);

#endif /* MICRO_OS_LIBC_USER_FORMAT_FLOAT_H */
