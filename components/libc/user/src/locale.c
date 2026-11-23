/*
 * Micro OS — the one locale of a program.
 *
 * The system is built without the tables of a locale: the C one is the whole
 * of it, and `setlocale` says so. The answer of `localeconv` is the structure
 * of the C locale, with a point for the radix and no grouping, which is what
 * `strtod` and the formatter of the library already do.
 */
#include "ulibc.h"

#include <locale.h>

static struct lconv g_conversion;

static char g_point[] = ".";
static char g_empty[] = "";
static char g_group[] = "";

char *setlocale(int category, const char *locale)
{
    (void)category;

    if (locale == NULL || locale[0] == '\0' || (locale[0] == 'C' && locale[1] == '\0')) {
        return "C";
    }

    return NULL;
}

struct lconv *localeconv(void)
{
    g_conversion.decimal_point = g_point;
    g_conversion.thousands_sep = g_empty;
    g_conversion.grouping = g_group;
    g_conversion.int_curr_symbol = g_empty;
    g_conversion.currency_symbol = g_empty;
    g_conversion.mon_decimal_point = g_empty;
    g_conversion.mon_thousands_sep = g_empty;
    g_conversion.mon_grouping = g_group;
    g_conversion.positive_sign = g_empty;
    g_conversion.negative_sign = g_empty;
    g_conversion.int_frac_digits = 127;
    g_conversion.frac_digits = 127;
    g_conversion.p_cs_precedes = 127;
    g_conversion.p_sep_by_space = 127;
    g_conversion.n_cs_precedes = 127;
    g_conversion.n_sep_by_space = 127;
    g_conversion.p_sign_posn = 127;
    g_conversion.n_sign_posn = 127;

    return &g_conversion;
}
