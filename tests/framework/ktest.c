#include "ktest.h"

#include <stdarg.h>

int ktest_checks;
int ktest_failures;
const char *ktest_current = "";

void ktest_begin(const char *name)
{
    ktest_current = name;
    printf("  - %s\n", name);
}

void ktest_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;

    ktest_failures++;
    printf("    FAIL %s:%d [%s] ", file, line, ktest_current ? ktest_current : "?");

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
    fflush(stdout);
}

int ktest_summary(void)
{
    printf("\n%d check(s), %d failure(s)\n", ktest_checks, ktest_failures);
    fflush(stdout);
    return ktest_failures == 0 ? 0 : 1;
}
