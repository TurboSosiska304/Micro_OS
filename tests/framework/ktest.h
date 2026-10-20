/*
 * Micro OS — minimal unit test framework.
 *
 * Deliberately dependency free: the test suite must build with nothing but a
 * C compiler, so that it can run on a CI machine and on a developer laptop.
 */
#ifndef MICRO_OS_KTEST_H
#define MICRO_OS_KTEST_H

#include <stdio.h>
#include <string.h>

extern int ktest_checks;
extern int ktest_failures;
extern const char *ktest_current;

void ktest_begin(const char *name);
void ktest_fail(const char *file, int line, const char *fmt, ...);
int ktest_summary(void);

#define KT_CHECK(cond)                                                                             \
    do {                                                                                           \
        ktest_checks++;                                                                            \
        if (!(cond)) {                                                                             \
            ktest_fail(__FILE__, __LINE__, "check failed: %s", #cond);                             \
        }                                                                                          \
    } while (0)

#define KT_CHECK_INT_EQ(actual, expected)                                                          \
    do {                                                                                           \
        long long a_ = (long long)(actual);                                                        \
        long long e_ = (long long)(expected);                                                      \
        ktest_checks++;                                                                            \
        if (a_ != e_) {                                                                            \
            ktest_fail(__FILE__, __LINE__, "%s == %lld, expected %lld", #actual, a_, e_);          \
        }                                                                                          \
    } while (0)

#define KT_CHECK_STR_EQ(actual, expected)                                                          \
    do {                                                                                           \
        const char *a_ = (actual);                                                                 \
        const char *e_ = (expected);                                                               \
        ktest_checks++;                                                                            \
        if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0) {                                     \
            ktest_fail(__FILE__, __LINE__, "%s == \"%s\", expected \"%s\"", #actual,               \
                       a_ ? a_ : "(null)", e_ ? e_ : "(null)");                                    \
        }                                                                                          \
    } while (0)

#define KT_CHECK_STR_NE(actual, expected)                                                          \
    do {                                                                                           \
        const char *a_ = (actual);                                                                 \
        const char *e_ = (expected);                                                               \
        ktest_checks++;                                                                            \
        if (a_ != NULL && e_ != NULL && strcmp(a_, e_) == 0) {                                     \
            ktest_fail(__FILE__, __LINE__, "%s == \"%s\", expected a different value", #actual,    \
                       a_);                                                                        \
        }                                                                                          \
    } while (0)

#endif /* MICRO_OS_KTEST_H */
