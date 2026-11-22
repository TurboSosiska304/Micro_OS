/*
 * Micro OS — the allocation and the conversions of a program.
 *
 * The names are the ones of the C library because a ported program includes
 * this header and not `ulibc.h`; the functions behind them are the arena of
 * `src/malloc.c`, which lives in the window of the program and never asks the
 * kernel for a block. The kernel's `KSYS_MALLOC` returns memory of ring 0, and
 * a program could not read a byte of it.
 *
 * The conversions are the ones an argument of a command needs: a signed and an
 * unsigned number in a base, a real number of a sort, and the sort itself. They
 * are the strict shape of `strtol`: the end of the conversion is written where
 * the caller asks and a number that does not fit is `ERANGE`.
 */
#ifndef MICRO_OS_LIBC_USER_STDLIB_H
#define MICRO_OS_LIBC_USER_STDLIB_H

#include "ulibc.h"

#define EXIT_SUCCESS ULIBC_EXIT_SUCCESS
#define EXIT_FAILURE ULIBC_EXIT_FAILURE

#ifndef RAND_MAX
#define RAND_MAX 32767
#endif

long strtol(const char *text, char **end, int base);
unsigned long strtoul(const char *text, char **end, int base);
long long strtoll(const char *text, char **end, int base);
unsigned long long strtoull(const char *text, char **end, int base);
double strtod(const char *text, char **end);
int atoi(const char *text);
long atol(const char *text);
long long atoll(const char *text);
int abs(int value);
long labs(long value);
long long llabs(long long value);

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));

char *getenv(const char *name);
int atexit(void (*function)(void));
void abort(void) __attribute__((noreturn));

#endif /* MICRO_OS_LIBC_USER_STDLIB_H */
