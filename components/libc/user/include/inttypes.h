/*
 * Micro OS — the format of an integer of a fixed width.
 *
 * A portable library prints a `uint64_t` with `PRIx64`, and the formatter of
 * this system speaks the `l`, `ll` and `z` lengths of the standard. The one
 * question the header has to answer is the length of `uint64_t`: on x86-64 a
 * `long` is 64 bits and the type is `unsigned long`, on Xtensa it is a `long
 * long`. `__SIZEOF_LONG__` is the compiler's own answer, so the two ports of
 * the system share the file.
 *
 * The `SCN` half is here for a source that reads with `sscanf`, which the
 * library of a program does not have yet (`docs/debts.md`); a caller that
 * names one is a link error and not a silent wrong conversion.
 */
#ifndef MICRO_OS_LIBC_USER_INTTYPES_H
#define MICRO_OS_LIBC_USER_INTTYPES_H

#include <stdint.h>

#if __SIZEOF_LONG__ == 8
#define MICRO_OS_PRINT64 "l"
#define MICRO_OS_PRINTPTR "l"
#elif __SIZEOF_LONG__ == 4
#define MICRO_OS_PRINT64 "ll"
#if __SIZEOF_POINTER__ == 8
#define MICRO_OS_PRINTPTR "l"
#else
#define MICRO_OS_PRINTPTR ""
#endif
#else
#error "a long that is neither 32 nor 64 bits is not a machine of this system"
#endif

#define PRId8 "d"
#define PRIi8 "i"
#define PRIu8 "u"
#define PRIo8 "o"
#define PRIx8 "x"
#define PRIX8 "X"

#define PRId16 "d"
#define PRIi16 "i"
#define PRIu16 "u"
#define PRIo16 "o"
#define PRIx16 "x"
#define PRIX16 "X"

#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIo32 "o"
#define PRIx32 "x"
#define PRIX32 "X"

#define PRId64 MICRO_OS_PRINT64 "d"
#define PRIi64 MICRO_OS_PRINT64 "i"
#define PRIu64 MICRO_OS_PRINT64 "u"
#define PRIo64 MICRO_OS_PRINT64 "o"
#define PRIx64 MICRO_OS_PRINT64 "x"
#define PRIX64 MICRO_OS_PRINT64 "X"

#define PRIdPTR MICRO_OS_PRINTPTR "d"
#define PRIiPTR MICRO_OS_PRINTPTR "i"
#define PRIuPTR MICRO_OS_PRINTPTR "u"
#define PRIoPTR MICRO_OS_PRINTPTR "o"
#define PRIxPTR MICRO_OS_PRINTPTR "x"
#define PRIXPTR MICRO_OS_PRINTPTR "X"

#define PRIdMAX MICRO_OS_PRINT64 "d"
#define PRIiMAX MICRO_OS_PRINT64 "i"
#define PRIuMAX MICRO_OS_PRINT64 "u"
#define PRIoMAX MICRO_OS_PRINT64 "o"
#define PRIxMAX MICRO_OS_PRINT64 "x"
#define PRIXMAX MICRO_OS_PRINT64 "X"

#define SCNd64 MICRO_OS_PRINT64 "d"
#define SCNi64 MICRO_OS_PRINT64 "i"
#define SCNu64 MICRO_OS_PRINT64 "u"
#define SCNo64 MICRO_OS_PRINT64 "o"
#define SCNx64 MICRO_OS_PRINT64 "x"

#endif /* MICRO_OS_LIBC_USER_INTTYPES_H */
