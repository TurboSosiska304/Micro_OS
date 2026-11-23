/*
 * Micro OS — the character classes of a program.
 *
 * The classes are the ones of the C locale: a byte is a letter, a digit, a
 * space or neither, and no locale of the machine changes that. A program of
 * this system reads the ASCII of the console and of a file, and a table of
 * 256 bits would only promise a set the kernel does not have.
 */
#ifndef MICRO_OS_LIBC_USER_CTYPE_H
#define MICRO_OS_LIBC_USER_CTYPE_H

int isalnum(int byte);
int isalpha(int byte);
int isascii(int byte);
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
