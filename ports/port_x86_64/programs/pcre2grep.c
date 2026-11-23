/*
 * Micro OS — pcre2grep, the lines of a source that match a regular expression.
 *
 * The program is the small demonstration of the vendored PCRE2
 * (third_party/pcre2): the pattern is compiled by the 8-bit library, and a
 * line of the source is one subject of `pcre2_match()`. The expression is a
 * real one - `.` is any character, `^` and `$` are the ends of the line, a
 * group in parentheses is numbered - and the options of the caller choose
 * the case, the inversion, the line number and the match itself.
 *
 * The line is read a block at a time and kept in a buffer of its own, the
 * way `grep` does it. A line longer than the buffer is searched and printed
 * as far as it fit; the rest is dropped, and a match that straddles the cut
 * is a limit of the program and not a silent answer to a shorter line.
 *
 * The heap of the program (components/libc/user/src/malloc.c) is the
 * allocation of the library: PCRE2 asks for the compiled pattern, the match
 * data and, for a pattern that needs it, a stack of frames.
 *
 * The messages are written with `write` and a number of our own: the
 * formatter of the library is another kilobyte of an image that is already
 * close to the 64 KiB a program may be (KSYS_MAX_IO), and a grep has four
 * numbers to print.
 */
#define PCRE2_CODE_UNIT_WIDTH 8
#include "pcre2.h"

#include "ulibc.h"

#define PCREGREP_CHUNK 512
#define PCREGREP_LINE_MAX 1024

struct pcregrep_line {
    char text[PCREGREP_LINE_MAX];
    size_t length;
};

struct pcregrep_options {
    int fold;   /* -i: the case of the pattern and the line */
    int invert; /* -v: print the lines that do not match */
    int number; /* -n: the number of the line */
    int only;   /* -o: the match instead of the line */
    int group;  /* -g N: the group of the match, with -o */
};

static unsigned long g_line_number;
static int g_show_number;
static int g_matched;

static void out_text(const char *text)
{
    (void)write(ULIBC_STDOUT, text, strlen(text));
}

static void out_unsigned(unsigned long value)
{
    char digits[20];
    int count = 0;

    if (value == 0) {
        (void)write(ULIBC_STDOUT, "0", 1);
        return;
    }

    while (value > 0) {
        digits[count++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    }

    while (count > 0) {
        count--;
        (void)write(ULIBC_STDOUT, &digits[count], 1);
    }
}

static void out_signed(long value)
{
    if (value < 0) {
        (void)write(ULIBC_STDOUT, "-", 1);
        out_unsigned((unsigned long)(-value));
    } else {
        out_unsigned((unsigned long)value);
    }
}

static void usage(void)
{
    out_text("usage: pcre2grep [-i] [-v] [-n] [-o] [-g N] <pattern> [file]...\n");
}

/* One line of the source, in the console. The port turns the newline into
 * the pair a terminal expects; the program writes one newline. */
static int print_line(const struct pcregrep_line *line)
{
    if (g_show_number) {
        out_unsigned(g_line_number);
        out_text(":");
    }

    if (write(ULIBC_STDOUT, line->text, line->length) != (long)line->length) {
        return -1;
    }

    if (write(ULIBC_STDOUT, "\n", 1) != 1) {
        return -1;
    }

    return 0;
}

static int print_span(const PCRE2_UCHAR *text, size_t from, size_t to)
{
    if (g_show_number) {
        out_unsigned(g_line_number);
        out_text(":");
    }

    if (write(ULIBC_STDOUT, text + from, to - from) != (long)(to - from)) {
        return -1;
    }

    if (write(ULIBC_STDOUT, "\n", 1) != 1) {
        return -1;
    }

    return 0;
}

/*
 * The matches of one line. A search that finds nothing answers 0; an error
 * of the engine is printed once and answers 1. The caller decides what to do
 * with a line that matched: print it, print the match, or skip it.
 */
static int search_line(const struct pcregrep_line *line, pcre2_code *code, pcre2_match_data *data,
                       const struct pcregrep_options *options)
{
    const PCRE2_UCHAR *text = (const PCRE2_UCHAR *)line->text;
    size_t start = 0;

    for (;;) {
        PCRE2_SIZE *ovector;
        int found = pcre2_match(code, text, line->length, start, 0, data, NULL);

        if (found == PCRE2_ERROR_NOMATCH) {
            return 0;
        }

        if (found < 0) {
            out_text("pcre2grep: match error ");
            out_signed(found);
            out_text("\n");
            return 1;
        }

        ovector = pcre2_get_ovector_pointer(data);
        g_matched = 1;

        if (options->only && options->group > 0) {
            if (options->group < found && ovector[2 * options->group] != PCRE2_UNSET) {
                size_t from = ovector[2 * options->group];
                size_t to = ovector[2 * options->group + 1];

                if (print_span(text, from, to) != 0) {
                    return 1;
                }
            }
        } else if (options->only) {
            if (print_span(text, ovector[0], ovector[1]) != 0) {
                return 1;
            }
        } else {
            return 2; /* the line matched; the caller prints it */
        }

        /* A match of zero bytes would find the same place for ever; the
         * search goes one byte on, as a grep of a line does. */
        if (ovector[1] > start) {
            start = ovector[1];
        } else if (ovector[1] < line->length) {
            start = ovector[1] + 1;
        } else {
            return 0;
        }
    }
}

static int pcregrep_stream(int fd, const char *name, pcre2_code *code, pcre2_match_data *data,
                           const struct pcregrep_options *options)
{
    char buffer[PCREGREP_CHUNK];
    struct pcregrep_line line;
    int status = 0;

    line.length = 0;
    g_line_number = 0;

    for (;;) {
        long got = read(fd, buffer, sizeof(buffer));
        long i;

        if (got < 0) {
            out_text("pcre2grep: ");
            out_text(name);
            out_text(": error ");
            out_signed(got);
            out_text("\n");
            return 2;
        }

        if (got == 0) {
            break;
        }

        for (i = 0; i < got; i++) {
            unsigned char byte = (unsigned char)buffer[i];

            if (byte == '\n') {
                int answer;

                g_line_number++;
                answer = search_line(&line, code, data, options);

                if (answer == 1) {
                    status = 2;
                } else if (answer == 2) {
                    if (!options->invert && print_line(&line) != 0) {
                        status = 2;
                    }
                } else if (answer == 0 && options->invert) {
                    g_matched = 1;
                    if (print_line(&line) != 0) {
                        status = 2;
                    }
                }

                line.length = 0;
                continue;
            }

            /* A line longer than the buffer is searched and printed as far
             * as it fit; the rest is dropped. */
            if (line.length + 1 < sizeof(line.text)) {
                line.text[line.length++] = (char)byte;
            }
        }
    }

    /* A last line without its newline is a line as well. */
    if (line.length > 0) {
        int answer;

        g_line_number++;
        answer = search_line(&line, code, data, options);

        if (answer == 1) {
            status = 2;
        } else if (answer == 2) {
            if (!options->invert && print_line(&line) != 0) {
                status = 2;
            }
        } else if (answer == 0 && options->invert) {
            g_matched = 1;
            if (print_line(&line) != 0) {
                status = 2;
            }
        }
    }

    return status;
}

static int pcregrep_one(const char *path, pcre2_code *code, pcre2_match_data *data,
                        const struct pcregrep_options *options)
{
    int fd;
    int status;

    if (strcmp(path, "-") == 0) {
        return pcregrep_stream(ULIBC_STDIN, "stdin", code, data, options);
    }

    fd = open(path, ULIBC_O_RDONLY);
    if (fd < 0) {
        out_text("pcre2grep: ");
        out_text(path);
        out_text(": error ");
        out_signed(fd);
        out_text("\n");
        return 2;
    }

    status = pcregrep_stream(fd, path, code, data, options);
    (void)close(fd);

    return status;
}

int main(int argc, char *const argv[], char *const envp[])
{
    struct pcregrep_options options;
    pcre2_match_data *data;
    pcre2_code *code;
    const char *pattern;
    uint32_t flags = 0;
    PCRE2_SIZE error_offset;
    int error_code;
    int first = 1;
    int status = 0;
    int i;

    (void)envp;

    options.fold = 0;
    options.invert = 0;
    options.number = 0;
    options.only = 0;
    options.group = 0;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *letters = argv[first] + 1;

        for (; *letters != '\0'; letters++) {
            if (*letters == 'i') {
                options.fold = 1;
            } else if (*letters == 'v') {
                options.invert = 1;
            } else if (*letters == 'n') {
                options.number = 1;
            } else if (*letters == 'o') {
                options.only = 1;
            } else if (*letters == 'u') {
                /* Unicode is not in the vendored build (config.h); the
                 * option is refused rather than silently ignored. */
                out_text("pcre2grep: this build has no Unicode support\n");
                return ULIBC_EXIT_FAILURE;
            } else if (*letters == 'g') {
                const char *value = letters + 1;

                if (*value == '\0') {
                    if (first + 1 >= argc) {
                        usage();
                        return ULIBC_EXIT_FAILURE;
                    }
                    value = argv[++first];
                }

                options.group = 0;
                while (*value >= '0' && *value <= '9') {
                    options.group = options.group * 10 + (*value - '0');
                    value++;
                }
                if (*value != '\0' || options.group <= 0) {
                    usage();
                    return ULIBC_EXIT_FAILURE;
                }
                options.only = 1;
                break;
            } else {
                usage();
                return ULIBC_EXIT_FAILURE;
            }
        }
        first++;
    }

    if (options.invert && options.only) {
        out_text("pcre2grep: -v and -o cannot be used together\n");
        return ULIBC_EXIT_FAILURE;
    }

    if (first == argc) {
        usage();
        return ULIBC_EXIT_FAILURE;
    }

    pattern = argv[first++];
    g_show_number = options.number;

    if (options.fold) {
        flags |= PCRE2_CASELESS;
    }

    code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, flags, &error_code,
                         &error_offset, NULL);
    if (code == NULL) {
        out_text("pcre2grep: pattern error ");
        out_signed(error_code);
        out_text(" at offset ");
        out_unsigned((unsigned long)error_offset);
        out_text("\n");
        return 2;
    }

    data = pcre2_match_data_create_from_pattern(code, NULL);
    if (data == NULL) {
        out_text("pcre2grep: out of memory\n");
        pcre2_code_free(code);
        return 2;
    }

    if (first == argc) {
        status = pcregrep_stream(ULIBC_STDIN, "stdin", code, data, &options);
    } else {
        for (i = first; i < argc; i++) {
            if (pcregrep_one(argv[i], code, data, &options) != 0) {
                status = 2;
            }
        }
    }

    pcre2_match_data_free(data);
    pcre2_code_free(code);

    if (status == 0 && !g_matched) {
        status = ULIBC_EXIT_FAILURE;
    }

    return status;
}
