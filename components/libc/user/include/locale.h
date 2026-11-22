/*
 * Micro OS — the locale of a program.
 *
 * The system has one locale, the C one: the radix character of a number is a
 * point, the thousands are not grouped, and the names of the months and the
 * days of `strftime` are the English ones of the standard. `setlocale` says so
 * and refuses every other name, which is the answer of a C library that was
 * built without the tables of a locale.
 */
#ifndef MICRO_OS_LIBC_USER_LOCALE_H
#define MICRO_OS_LIBC_USER_LOCALE_H

#include "ulibc.h"

#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5

struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;
    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;
    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
};

char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#endif /* MICRO_OS_LIBC_USER_LOCALE_H */
