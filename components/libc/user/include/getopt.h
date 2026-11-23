/*
 * Micro OS — the options of a line, for a portable source.
 *
 * The parser is the one of POSIX: `getopt` walks the words of `argv` that
 * begin with a `-`, a word of several options is read letter by letter, and an
 * option that takes a value takes the rest of its word or the next one. The
 * three globals are the state of the walk; `optopt` is the letter of an
 * option the caller did not declare.
 *
 * `getopt_long` and `getopt_long_only` read the names of `struct option` as
 * well: `--name`, `--name=value` and `--name value`, and a prefix of a name
 * that no other name has. An option whose `flag` is not NULL stores its `val`
 * there and answers 0, and `longindex` is the place of the name in the table.
 * `getopt_long_only` tries the long names of a word with one dash first and
 * falls back to the letters of `getopt`, which is the last word of GNU.
 */
#ifndef MICRO_OS_LIBC_USER_GETOPT_H
#define MICRO_OS_LIBC_USER_GETOPT_H

#include <stddef.h>

/* The `has_arg` of the table: an option with no value, one that needs it, and
 * one that takes it only in its own word (`--name=value`). */
#define no_argument 0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *options);
int getopt_long(int argc, char *const argv[], const char *options, const struct option *longopts,
                int *longindex);
int getopt_long_only(int argc, char *const argv[], const char *options,
                     const struct option *longopts, int *longindex);

#endif /* MICRO_OS_LIBC_USER_GETOPT_H */
