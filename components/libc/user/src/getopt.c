/*
 * Micro OS — the options of a line, the parser of POSIX.
 *
 * `getopt` walks the words that begin with `-`, one letter at a time, and
 * stops at the first word that does not (or after `--`). An option of the
 * `options` string that is followed by `:` takes a value: the rest of its
 * word, or the next word. A letter the caller did not declare answers `?` and
 * `optopt` holds it; a missing value answers `?` as well, or `:` when the
 * options string begins with one. The three globals are the state of the
 * walk and are the ones of POSIX.
 *
 * A command of this tree parses its line with `ARGBEGIN` of sbase, and this
 * parser is here for the sources that were written against `<unistd.h>`. It
 * does not permute the words: the walk stops at the first operand, which is
 * the behaviour POSIX names and the one a script relies on.
 */
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

char *optarg;
int optind = 1;
int opterr = 1;
int optopt;

/* The place in a word of several options; NULL starts a new word. */
static char *g_next_s;

int getopt(int argc, char *const argv[], const char *options)
{
    const char *place;
    int leading_colon = (options[0] == ':');
    char letter;

    optarg = NULL;

    if (g_next_s == NULL || *g_next_s == '\0') {
        if (optind >= argc) {
            return -1;
        }

        if (argv[optind][0] != '-' || argv[optind][1] == '\0') {
            return -1; /* an operand, or the "-" of the standard input */
        }

        if (strcmp(argv[optind], "--") == 0) {
            optind++;
            return -1;
        }

        g_next_s = argv[optind] + 1;
        optind++;
    }

    letter = *g_next_s++;

    place = strchr(options, letter);
    if (letter == ':' || place == NULL) {
        optopt = (unsigned char)letter;
        if (opterr && !leading_colon) {
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], letter);
        }
        return '?';
    }

    if (place[1] == ':') {
        if (*g_next_s != '\0') {
            optarg = g_next_s;
            g_next_s = NULL;
        } else if (optind < argc) {
            optarg = argv[optind++];
            g_next_s = NULL;
        } else {
            g_next_s = NULL;
            optopt = (unsigned char)letter;
            if (leading_colon) {
                return ':';
            }
            if (opterr) {
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], letter);
            }
            return '?';
        }
    } else if (*g_next_s == '\0') {
        g_next_s = NULL;
    }

    return (unsigned char)letter;
}
