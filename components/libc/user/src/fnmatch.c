/*
 * Micro OS — the pattern of a name, for `find -name`.
 *
 * The grammar is the one of POSIX: `*` is any run of bytes, `?` one byte,
 * `[...]` a set of bytes with ranges and a leading `!` or `^` that inverts it,
 * and a backslash quotes the byte after it. `FNM_PATHNAME` keeps `*` and `?`
 * from crossing a slash and lets a slash match only itself; `FNM_NOESCAPE`
 * turns the backslash into an ordinary byte. A period has no special place in
 * this grammar, so `FNM_PERIOD` is accepted and changes nothing, which the
 * standard allows a machine whose names have no hidden convention.
 *
 * The match is a backtracking walk: a `*` tries the rest of the pattern at
 * every place it could end, and a `[...]` is read one byte at a time. The
 * patterns of a line are short and the names of this system are at most 31
 * bytes, so the walk costs nothing worth a compiled form.
 */
#include <fnmatch.h>

#include <ctype.h>
#include <string.h>

/* Is the byte one of the class `[:name:]`? */
static int class_match(const char *name, size_t length, unsigned char byte)
{
    static const char *const alpha = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *const digit = "0123456789";
    static const char *const alnum =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static const char *const lower = "abcdefghijklmnopqrstuvwxyz";
    static const char *const upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *const space = " \t\n\r\v\f";
    static const char *const blank = " \t";
    static const char *const punct = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
    static const char *const xdigit = "0123456789abcdefABCDEF";

    if (length == 5 && strncmp(name, "alpha", 5) == 0) {
        return strchr(alpha, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "digit", 5) == 0) {
        return strchr(digit, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "alnum", 5) == 0) {
        return strchr(alnum, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "lower", 5) == 0) {
        return strchr(lower, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "upper", 5) == 0) {
        return strchr(upper, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "space", 5) == 0) {
        return strchr(space, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "blank", 5) == 0) {
        return strchr(blank, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "punct", 5) == 0) {
        return strchr(punct, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "cntrl", 5) == 0) {
        return iscntrl(byte);
    }
    if (length == 6 && strncmp(name, "xdigit", 6) == 0) {
        return strchr(xdigit, byte) != NULL;
    }
    if (length == 5 && strncmp(name, "print", 5) == 0) {
        return isprint(byte);
    }
    if (length == 5 && strncmp(name, "graph", 5) == 0) {
        return isgraph(byte);
    }

    return -1; /* not a class of this engine */
}

/*
 * Match one byte against the set that starts after `[`. `*end` is the place
 * of the closing `]`; the caller has checked that there is one.
 */
static int set_match(const char *pattern, const char **end, unsigned char byte, int flags)
{
    const char *place = pattern;
    int invert = 0;
    int found = 0;

    if (*place == '!' || *place == '^') {
        invert = 1;
        place++;
    }

    /* A `]` at the start of the set is a byte of the set, not its end. */
    if (*place == ']') {
        if (byte == ']') {
            found = 1;
        }
        place++;
    }

    while (*place != '\0' && *place != ']') {
        unsigned char first;

        if (*place == '[' && place[1] == ':') {
            const char *close = strstr(place + 2, ":]");
            int answer;

            if (close == NULL) {
                /* Not a class after all: a literal `[`. */
                first = (unsigned char)*place++;
                if (first == byte) {
                    found = 1;
                }
                continue;
            }

            answer = class_match(place + 2, (size_t)(close - (place + 2)), byte);
            if (answer < 0) {
                /* An unknown class: the name is the set of the bytes of its
                 * text, which is what a reader of the pattern would see. */
                for (const char *letter = place; letter < close + 2; letter++) {
                    if ((unsigned char)*letter == byte) {
                        found = 1;
                    }
                }
            } else if (answer) {
                found = 1;
            }
            place = close + 2;
            continue;
        }

        if (*place == '\\' && (flags & FNM_NOESCAPE) == 0 && place[1] != '\0') {
            place++;
        }
        first = (unsigned char)*place++;

        if (*place == '-' && place[1] != ']' && place[1] != '\0') {
            unsigned char last;

            place++;
            if (*place == '\\' && (flags & FNM_NOESCAPE) == 0 && place[1] != '\0') {
                place++;
            }
            last = (unsigned char)*place++;
            if (byte >= first && byte <= last) {
                found = 1;
            }
        } else if (byte == first) {
            found = 1;
        }
    }

    *end = place + ((*place == ']') ? 1 : 0);

    return found != invert;
}

static int match(const char *pattern, const char *text, int flags);

/* The rest of the text after a byte, or NULL when the byte may not be read
 * because the name reached a slash under FNM_PATHNAME. */
static const char *next_byte(const char *text)
{
    return text + 1;
}

static int match(const char *pattern, const char *text, int flags)
{
    while (*pattern != '\0') {
        switch (*pattern) {
        case '?': {
            if (*text == '\0' || ((flags & FNM_PATHNAME) != 0 && *text == '/')) {
                return FNM_NOMATCH;
            }
            pattern++;
            text = next_byte(text);
            break;
        }
        case '*': {
            const char *rest = pattern;

            while (*rest == '*') {
                rest++;
            }
            if (*rest == '\0') {
                /* A trailing `*` matches the rest, except that a slash has to
                 * be matched by a slash under FNM_PATHNAME. */
                return ((flags & FNM_PATHNAME) != 0 && strchr(text, '/') != NULL) ? FNM_NOMATCH : 0;
            }

            for (;;) {
                if (match(rest, text, flags) == 0) {
                    return 0;
                }
                if (*text == '\0' || ((flags & FNM_PATHNAME) != 0 && *text == '/')) {
                    return FNM_NOMATCH;
                }
                text = next_byte(text);
            }
        }
        case '[': {
            const char *place = pattern + 1;
            const char *end = NULL;
            const char *search = place;

            while (*search != '\0') {
                if (*search == '\\' && (flags & FNM_NOESCAPE) == 0 && search[1] != '\0') {
                    search += 2;
                    continue;
                }
                if (*search == ']') {
                    break;
                }
                search++;
            }
            if (*search != ']') {
                /* An unclosed set is a literal `[`, as the standard asks. */
                if (*text != '[') {
                    return FNM_NOMATCH;
                }
                pattern++;
                text = next_byte(text);
                break;
            }

            if (*text == '\0' || ((flags & FNM_PATHNAME) != 0 && *text == '/')) {
                return FNM_NOMATCH;
            }
            if (!set_match(place, &end, (unsigned char)*text, flags)) {
                return FNM_NOMATCH;
            }
            pattern = end;
            text = next_byte(text);
            break;
        }
        case '\\':
            if ((flags & FNM_NOESCAPE) == 0 && pattern[1] != '\0') {
                pattern++;
            }
            /* fall through */
        default:
            if (*pattern != *text) {
                return FNM_NOMATCH;
            }
            pattern++;
            text = next_byte(text);
            break;
        }
    }

    return (*text == '\0') ? 0 : FNM_NOMATCH;
}

int fnmatch(const char *pattern, const char *text, int flags)
{
    if (pattern == NULL || text == NULL) {
        return FNM_NOMATCH;
    }

    return match(pattern, text, flags);
}
