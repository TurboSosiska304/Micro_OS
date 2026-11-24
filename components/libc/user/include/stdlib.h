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
double atof(const char *text);
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
/*
 * The environment a program may grow: `setenv` adds or replaces a name,
 * `unsetenv` removes every word of it, and `putenv` takes a word of the
 * caller ("NAME=VALUE") as it is and never copies or frees it. A name is not
 * empty and has no '=', and a call that is given one answers `EINVAL`.
 */
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *word);
int atexit(void (*function)(void));
void abort(void) __attribute__((__noreturn__));

/*
 * A command of the machine as a process of its own: `system` forks the child,
 * puts the program of the first word of the line over it and waits. There is
 * no `/bin/sh` on this system, so the line is split by the quoting rules of
 * the shell and the first word names the program (`src/words.c`,
 * `src/command.c`). A NULL line answers 1, as POSIX asks ("a shell is
 * available"); the status of the command is the number the child exited with.
 */
int system(const char *command);

#endif /* MICRO_OS_LIBC_USER_STDLIB_H */
