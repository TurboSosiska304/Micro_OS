/*
 * Micro OS — the strings of a program, under the name the C library uses.
 *
 * The library of a program is `ulibc.h`; this header is the door a ported
 * program expects when it writes `#include <string.h>`. It declares nothing
 * of its own: every function is the one of the library, so there is one
 * implementation on the machine and not two.
 */
#ifndef MICRO_OS_LIBC_USER_STRING_H
#define MICRO_OS_LIBC_USER_STRING_H

#include "ulibc.h"

#endif /* MICRO_OS_LIBC_USER_STRING_H */
