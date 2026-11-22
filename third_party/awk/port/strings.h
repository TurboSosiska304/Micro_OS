/*
 * Micro OS — the compare of two words without the case, for the One True Awk.
 *
 * The source includes `<strings.h>` for `strncasecmp` (the name of `nan` and
 * `inf` in a number is read without the case). The function is the one of the
 * port layer, not of the library of the program; see `port/awk_port.h`.
 */
#ifndef MICRO_OS_THIRD_PARTY_AWK_PORT_STRINGS_H
#define MICRO_OS_THIRD_PARTY_AWK_PORT_STRINGS_H

#include "ulibc.h"

int strncasecmp(const char *left, const char *right, size_t count);

#endif /* MICRO_OS_THIRD_PARTY_AWK_PORT_STRINGS_H */
