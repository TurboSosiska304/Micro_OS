/*
 * Micro OS — the formatter of a double, built for the host.
 *
 * The source of the guest is `components/libc/user/src/format_float.c`, and it
 * is compiled here with the headers of the guest into a translation unit of
 * its own: the test binary is built against the C library of the host, and
 * the type `FILE` of the two libraries is not the same type. The shim exposes
 * one function with plain arguments and no type of the guest in its
 * prototype, so the test file includes the framework and nothing else.
 */
#include <stddef.h>

int microos_test_double_text(char *out, size_t cap, double value, char conversion, int precision,
                             int plus, int space, int alternate);

#define ULIBC_WITH_FLOAT 1
#include "../../components/libc/user/src/format_float.c"

int microos_test_double_text(char *out, size_t cap, double value, char conversion, int precision,
                             int plus, int space, int alternate)
{
    return ulibc_double_text(out, cap, value, conversion, precision, plus, space, alternate);
}
