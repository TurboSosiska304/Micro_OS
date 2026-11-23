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
 * `getopt_long` is the same walk with the long names of `struct option`:
 * `--name`, `--name=value` and `--name value`, the value of an option that
 * asks for one and no value of an option that does not. A name may be
 * shortened to any prefix that no other name has (`--na` for `--name`); an
 * ambiguous prefix is an error and so is a name nobody declared. An option
 * with `flag` set writes its `val` there and answers 0, as the standard says,
 * and `longindex` is the place of the name in the table. `getopt_long_only`
 * reads a word of one dash as a long name first and falls back to the letters
 * of `getopt` when no name matches.
 *
 * A command of this tree parses its line with `ARGBEGIN` of sbase, and these
 * parsers are here for the sources that were written against `<unistd.h>`
 * and `<getopt.h>`. They do not permute the words: the walk stops at the
 * first operand, which is the behaviour POSIX names and the one a script
 * relies on.
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

/*
 * The place of a long name in the table: 1 with its index, 0 when no name
 * begins with the text and -1 when more than one does. An exact name wins
 * over the prefixes, whatever the order of the table.
 */
static int long_find(const char *name, size_t length, const struct option *longopts, int *index)
{
    int found = -1;
    int prefixes = 0;
    int at;

    for (at = 0; longopts[at].name != NULL; at++) {
        size_t other = strlen(longopts[at].name);

        if (other < length || strncmp(longopts[at].name, name, length) != 0) {
            continue;
        }
        if (other == length) {
            *index = at;
            return 1;
        }
        prefixes++;
        found = at;
    }

    if (prefixes == 1) {
        *index = found;
        return 1;
    }

    return (prefixes > 1) ? -1 : 0;
}

/*
 * One word of the long form, or -2 when the word is not one and `getopt` has
 * to read it. The caller advanced `optind` past the word when the answer is
 * not -2.
 */
static int long_option(int argc, char *const argv[], const struct option *longopts, int *longindex,
                       int only)
{
    const char *word = argv[optind];
    const char *body;
    const char *equal;
    const struct option *found;
    size_t length;
    char *value = NULL;
    int index = -1;
    int match;

    if (word[0] != '-' || word[1] == '\0') {
        return -2;
    }
    if (word[1] == '-') {
        if (word[2] == '\0') {
            optind++; /* the `--` of the end of the options */
            return -1;
        }
        body = word + 2;
    } else if (only) {
        body = word + 1;
    } else {
        return -2;
    }

    equal = strchr(body, '=');
    length = (equal != NULL) ? (size_t)(equal - body) : strlen(body);
    if (length == 0) {
        return -2;
    }

    match = long_find(body, length, longopts, &index);
    if (match == 0 && only && word[1] != '-') {
        return -2; /* not a name: the letters of `getopt` read the word */
    }
    optarg = NULL;
    optopt = 0;

    if (match != 1) {
        if (opterr) {
            if (match == 0) {
                fprintf(stderr, "%s: unrecognized option '%.*s'\n", argv[0], (int)length, body);
            } else {
                fprintf(stderr, "%s: option '%.*s' is ambiguous\n", argv[0], (int)length, body);
            }
        }
        optind++;
        return '?';
    }

    found = &longopts[index];
    if (found->has_arg == no_argument && equal != NULL) {
        if (opterr) {
            fprintf(stderr, "%s: option '%.*s' doesn't allow an argument\n", argv[0], (int)length,
                    body);
        }
        optind++;
        return '?';
    }
    if (equal != NULL) {
        value = (char *)(equal + 1);
    } else if (found->has_arg == required_argument) {
        if (optind + 1 < argc) {
            value = argv[optind + 1];
            optind++;
        } else {
            if (opterr) {
                fprintf(stderr, "%s: option '%.*s' requires an argument\n", argv[0], (int)length,
                        body);
            }
            optind++;
            return '?';
        }
    }

    optind++;
    optarg = value;
    if (longindex != NULL) {
        *longindex = index;
    }
    if (found->flag != NULL) {
        *found->flag = found->val;
        return 0;
    }

    return found->val;
}

int getopt_long(int argc, char *const argv[], const char *options, const struct option *longopts,
                int *longindex)
{
    int answer;

    optarg = NULL;

    if (g_next_s == NULL || *g_next_s == '\0') {
        g_next_s = NULL;
        if (optind >= argc) {
            return -1;
        }

        answer = long_option(argc, argv, longopts, longindex, 0);
        if (answer != -2) {
            return answer;
        }
    }

    return getopt(argc, argv, options);
}

int getopt_long_only(int argc, char *const argv[], const char *options,
                     const struct option *longopts, int *longindex)
{
    int answer;

    optarg = NULL;

    if (g_next_s == NULL || *g_next_s == '\0') {
        g_next_s = NULL;
        if (optind >= argc) {
            return -1;
        }

        answer = long_option(argc, argv, longopts, longindex, 1);
        if (answer != -2) {
            return answer;
        }
    }

    return getopt(argc, argv, options);
}
