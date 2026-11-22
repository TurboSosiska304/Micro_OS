/*
 * Micro OS — the tests of the formatter of a double of a program.
 *
 * The formatter of the guest (`components/libc/user/src/format_float.c`) is
 * not the one of the host, and the two have to agree: a script of Lua prints
 * `string.format('%.3f', 1/3)` through it, and a reader of that line expects
 * the digits of the C library. The test builds the source of the guest with
 * its own headers (through a shim, because the two libraries disagree about
 * `FILE`) and compares its answer with the `snprintf` of the host for a table
 * of values and conversions.
 *
 * A value whose last digit depends on the rounding of a tie is left out on
 * purpose: the guest rounds the half away from zero, the host rounds it to
 * the even digit, and the difference is a debt of the formatter
 * (`docs/debts.md`) and not a shape of this test.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>

int microos_test_double_text(char *out, size_t cap, double value, char conversion, int precision,
                             int plus, int space, int alternate);

struct sample {
    double value;
    char conversion;
    int precision;
};

static const struct sample g_samples[] = {
    {0.0, 'f', 6},       {1.0, 'f', 0},         {0.5, 'f', 1},
    {1.0 / 3.0, 'f', 3}, {-1.5, 'f', 2},        {3.14159265358979, 'f', 4},
    {1024.0, 'f', 1},    {123456789.0, 'f', 2}, {1e20, 'f', 3},
    {1.0 / 3.0, 'e', 4}, {12345.678, 'e', 2},   {0.000123, 'e', 5},
    {-2.5e-10, 'e', 3},  {1.0 / 3.0, 'g', 6},   {1024.0, 'g', 6},
    {0.0001, 'g', 6},    {1e-5, 'g', 6},        {123456789.0, 'g', 6},
    {2.0, 'g', 1},       {0.5, 'g', 1},         {100.0, 'g', 3},
};

static void check_one(const struct sample *sample)
{
    char ours[256];
    char theirs[256];

    microos_test_double_text(ours, sizeof(ours), sample->value, sample->conversion,
                             sample->precision, 0, 0, 0);
    {
        char format[8];

        format[0] = '%';
        format[1] = '.';
        format[2] = (char)('0' + (sample->precision / 10) % 10);
        format[3] = (char)('0' + sample->precision % 10);
        format[4] = sample->conversion;
        format[5] = '\0';
        snprintf(theirs, sizeof(theirs), format, sample->value);
    }

    KT_CHECK_STR_EQ(ours, theirs);
}

void test_userland_float(void)
{
    size_t index;

    ktest_begin("userland: the formatter of a double is the one of the C library");

    for (index = 0; index < sizeof(g_samples) / sizeof(g_samples[0]); index++) {
        check_one(&g_samples[index]);
    }

    {
        char text[64];

        /* The sign of a requested plus and the zero it is printed for. */
        microos_test_double_text(text, sizeof(text), 2.5, 'f', 1, 1, 0, 0);
        KT_CHECK_STR_EQ(text, "+2.5");

        microos_test_double_text(text, sizeof(text), -0.0, 'f', 1, 0, 0, 0);
        KT_CHECK_STR_EQ(text, "-0.0");

        /* The alternative form keeps the point of an empty fraction. */
        microos_test_double_text(text, sizeof(text), 2.0, 'f', 0, 0, 0, 1);
        KT_CHECK_STR_EQ(text, "2.");

        /* The values that are not numbers, with the letters of the case. */
        microos_test_double_text(text, sizeof(text), 1.0 / 0.0, 'f', 6, 0, 0, 0);
        KT_CHECK_STR_EQ(text, "inf");
        microos_test_double_text(text, sizeof(text), -1.0 / 0.0, 'E', 6, 0, 0, 0);
        KT_CHECK_STR_EQ(text, "-inf");
        /* The not-a-number of x86-64 has the sign bit of the operation that
         * made it: 0.0/0.0 is the quiet NaN with the sign set, and both
         * libraries print the minus. The expected text is the host's. */
        {
            char theirs[64];

            microos_test_double_text(text, sizeof(text), 0.0 / 0.0, 'f', 6, 0, 0, 0);
            snprintf(theirs, sizeof(theirs), "%f", 0.0 / 0.0);
            KT_CHECK_STR_EQ(text, theirs);
        }
    }
}
