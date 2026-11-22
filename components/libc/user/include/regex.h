/*
 * Micro OS — the regular expressions of `grep` and `sed`.
 *
 * The engine is the one of `src/regex.c`: a basic or extended expression of
 * POSIX - the literals, `.`, `*`, `^`, `$`, a bracket expression, a group and
 * a back reference - compiled to a tree and matched from the left. It is not
 * the engine of a Unix-like system with a locale: a class is the ASCII one,
 * and a multibyte character is a run of bytes.
 *
 * `REG_NOSUB` says the caller does not want the offsets; `REG_NEWLINE` makes
 * `.` and a negated class stop at a newline and `^`/`$` match beside one;
 * `REG_NOTBOL`/`REG_NOTEOL` say the text is not the beginning/end of a line;
 * `REG_ICASE` folds the case of a byte of ASCII.
 */
#ifndef MICRO_OS_LIBC_USER_REGEX_H
#define MICRO_OS_LIBC_USER_REGEX_H

#include <stddef.h>

typedef long regoff_t;

typedef struct {
    size_t re_nsub;
    void *re_private;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

#define REG_EXTENDED (1 << 0)
#define REG_ICASE (1 << 1)
#define REG_NEWLINE (1 << 2)
#define REG_NOSUB (1 << 3)
#define REG_NOTBOL (1 << 4)
#define REG_NOTEOL (1 << 5)

#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13

int regcomp(regex_t *expression, const char *pattern, int flags);
int regexec(const regex_t *expression, const char *text, size_t count, regmatch_t matches[],
            int flags);
size_t regerror(int code, const regex_t *expression, char *buffer, size_t size);
void regfree(regex_t *expression);

#endif /* MICRO_OS_LIBC_USER_REGEX_H */
