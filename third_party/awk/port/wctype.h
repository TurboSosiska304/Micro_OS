/*
 * Micro OS — the wide characters of the C locale, for the One True Awk.
 *
 * The release of 2024 folds the case of a string with `towupper`/`towlower`
 * when MB_CUR_MAX is more than one. This system has the C locale and one byte
 * to a character (`port/awk_port.h`), so those functions are the ASCII ones
 * and the branch is never taken; the header is here because `run.c` includes
 * `<wctype.h>` and the compiler wants the names.
 */
#ifndef MICRO_OS_THIRD_PARTY_AWK_PORT_WCTYPE_H
#define MICRO_OS_THIRD_PARTY_AWK_PORT_WCTYPE_H

#include <stddef.h>

typedef unsigned int wint_t;

#define WEOF ((wint_t)-1)

wint_t towupper(wint_t wide);
wint_t towlower(wint_t wide);

#endif /* MICRO_OS_THIRD_PARTY_AWK_PORT_WCTYPE_H */
