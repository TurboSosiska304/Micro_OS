/*
 * Micro OS — the options of a line, for a portable source.
 *
 * The parser is the one of POSIX: `getopt` walks the words of `argv` that
 * begin with a `-`, a word of several options is read letter by letter, and an
 * option that takes a value takes the rest of its word or the next one. The
 * three globals are the state of the walk; `optopt` is the letter of an
 * option the caller did not declare.
 */
#ifndef MICRO_OS_LIBC_USER_GETOPT_H
#define MICRO_OS_LIBC_USER_GETOPT_H

#include <stddef.h>

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *options);

#endif /* MICRO_OS_LIBC_USER_GETOPT_H */
