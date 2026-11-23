/*
 * Micro OS — the size of a file in the units of `ls -h`.
 *
 * Upstream `libutil/human.c` keeps the size in a `double` and prints it with
 * `%.1f`; the guest of this system has no floating point registers, so the
 * port answers the same shape with integers: the size divided by 1024 until
 * it is below 1024, and the tenths from the integer remainder. The answer is
 * in a static buffer, as the function of the collection answers it.
 */
#include <stdio.h>

#include "../util.h"

char *humansize(off_t size)
{
    static char buffer[24];
    static const char postfixes[] = "BKMGTPE";
    unsigned long long value = (size < 0) ? (unsigned long long)0 - (unsigned long long)size
                                          : (unsigned long long)size;
    unsigned long long whole;
    unsigned long long tenths;
    int unit = 0;

    if (size < 0) {
        (void)snprintf(buffer, sizeof(buffer), "-");
        value = (unsigned long long)0 - (unsigned long long)size;
    }

    if (value < 1024ull) {
        if (size < 0) {
            (void)snprintf(buffer, sizeof(buffer), "-%llu", value);
            return buffer;
        }
        (void)snprintf(buffer, sizeof(buffer), "%llu", value);
        return buffer;
    }

    while (value >= 1024ull * 1024ull && postfixes[unit + 1] != '\0') {
        value /= 1024ull;
    }
    if (value >= 1024ull) {
        /* The last division leaves the tenths: 1536 becomes 1.5K. */
        whole = value / 1024ull;
        tenths = ((value % 1024ull) * 10ull + 512ull) / 1024ull;
        if (unit + 1 < (int)sizeof(postfixes) - 1) {
            unit++;
        }
    } else {
        whole = value;
        tenths = 0;
    }

    if (tenths == 10) {
        whole++;
        tenths = 0;
    }

    if (size < 0) {
        (void)snprintf(buffer, sizeof(buffer), "-%llu.%llu%c", whole, tenths, postfixes[unit]);
    } else {
        (void)snprintf(buffer, sizeof(buffer), "%llu.%llu%c", whole, tenths, postfixes[unit]);
    }

    return buffer;
}
