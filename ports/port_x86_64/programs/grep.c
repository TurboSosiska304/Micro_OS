/*
 * Micro OS — grep, the lines of a source that hold a word.
 *
 * The program reads a source a block at a time and keeps the current line in
 * a buffer of its own; when the line ends, it is printed if the word is in
 * it (or is not in it, with `-v`), and the next one begins. The search is a
 * substring of the line and not an expression: `strstr` of the library is
 * the whole engine. `-i` folds the case of the two sides, one ASCII letter
 * at a time, because the library has no `ctype` and a program of this size
 * does not need one.
 *
 * A line longer than the buffer is searched and printed as far as it fit,
 * and the rest of it is dropped: a word that straddles the cut may be
 * missed, which is a limit of the program and not a silent answer to a
 * shorter line. The sources are the files of the line; no name at all, or
 * `-`, is descriptor 0, the input of the program.
 */
#include "ulibc.h"

#include "kernel/errno.h"

#define GREP_CHUNK 512
#define GREP_LINE_MAX 256

struct grep_line {
    char text[GREP_LINE_MAX];
    size_t length;
};

static int lower(int letter)
{
    if (letter >= 'A' && letter <= 'Z') {
        return letter - 'A' + 'a';
    }

    return letter;
}

static int holds(const char *line, const char *word, int fold)
{
    size_t length = strlen(word);
    const char *start;

    if (length == 0) {
        return 1; /* the empty word is in every line */
    }

    if (!fold) {
        return strstr(line, word) != NULL;
    }

    for (start = line; *start != '\0'; start++) {
        size_t i;

        for (i = 0; i < length; i++) {
            if (lower((unsigned char)start[i]) != lower((unsigned char)word[i])) {
                break;
            }
        }

        if (i == length) {
            return 1;
        }
    }

    return 0;
}

static int emit(const struct grep_line *line, const char *word, int fold, int invert)
{
    int match = holds(line->text, word, fold);

    if (invert) {
        match = !match;
    }

    if (!match) {
        return 0;
    }

    if (write(ULIBC_STDOUT, line->text, line->length) != (long)line->length) {
        return -1;
    }

    if (write(ULIBC_STDOUT, "\n", 1) != 1) {
        return -1;
    }

    return 0;
}

static int grep_stream(int fd, const char *name, const char *word, int fold, int invert)
{
    char buffer[GREP_CHUNK];
    struct grep_line line;

    line.length = 0;

    for (;;) {
        long got = read(fd, buffer, sizeof(buffer));
        long i;

        if (got < 0) {
            printf("grep: %s: error %ld\n", name, -got);
            return 1;
        }

        if (got == 0) {
            break;
        }

        for (i = 0; i < got; i++) {
            unsigned char byte = (unsigned char)buffer[i];

            if (byte == '\n') {
                line.text[line.length] = '\0';
                if (emit(&line, word, fold, invert) != 0) {
                    printf("grep: %s: error %d\n", name, KERR_IO);
                    return 1;
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
        line.text[line.length] = '\0';
        if (emit(&line, word, fold, invert) != 0) {
            printf("grep: %s: error %d\n", name, KERR_IO);
            return 1;
        }
    }

    return 0;
}

static int grep_one(const char *path, const char *word, int fold, int invert)
{
    int status;
    int fd;

    if (strcmp(path, "-") == 0) {
        return grep_stream(ULIBC_STDIN, "stdin", word, fold, invert);
    }

    fd = open(path, ULIBC_O_RDONLY);
    if (fd < 0) {
        printf("grep: %s: error %d\n", path, -fd);
        return 1;
    }

    status = grep_stream(fd, path, word, fold, invert);
    (void)close(fd);

    return status;
}

int main(int argc, char *const argv[], char *const envp[])
{
    int fold = 0;
    int invert = 0;
    int first = 1;
    int status = 0;
    const char *word;
    int i;

    (void)envp;

    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *letter = argv[first] + 1;

        for (; *letter != '\0'; letter++) {
            if (*letter == 'i') {
                fold = 1;
            } else if (*letter == 'v') {
                invert = 1;
            } else {
                printf("usage: grep [-i] [-v] <word> [file]...\n");
                return ULIBC_EXIT_FAILURE;
            }
        }
        first++;
    }

    if (first == argc) {
        printf("usage: grep [-i] [-v] <word> [file]...\n");
        return ULIBC_EXIT_FAILURE;
    }

    word = argv[first++];

    if (first == argc) {
        return grep_stream(ULIBC_STDIN, "stdin", word, fold, invert);
    }

    for (i = first; i < argc; i++) {
        if (grep_one(argv[i], word, fold, invert) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
