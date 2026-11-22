/*
 * Micro OS — the classes of a byte.
 *
 * The classes are the ones of the C locale and the argument is a byte of the
 * program, not a wide character. The functions are the usual ones because a
 * portable source calls them; `lctype.c` of Lua does not, it carries a table
 * of its own for the lexer.
 */
#ifndef MICRO_OS_LIBC_USER_CTYPE_H
#define MICRO_OS_LIBC_USER_CTYPE_H

#include "ulibc.h"

int isascii(int byte);
int isalnum(int byte);
int isalpha(int byte);
int isblank(int byte);
int iscntrl(int byte);
int isdigit(int byte);
int isgraph(int byte);
int islower(int byte);
int isprint(int byte);
int ispunct(int byte);
int isspace(int byte);
int isupper(int byte);
int isxdigit(int byte);
int tolower(int byte);
int toupper(int byte);

#endif /* MICRO_OS_LIBC_USER_CTYPE_H */
