/*
 * Micro OS — wc, the counter of a stream.
 *
 * The program exists for the second half of a pipe: `ls /bin | /bin/wc -l`
 * is a producer of the shell and a consumer that reads a descriptor the
 * shell built, and the number it prints could not come from anywhere but
 * those bytes. It counts the lines, the words and the bytes of its input;
 * with no name that input is descriptor 0, and `-` names the same stream.
 *
 * It walks the buffer without the character classes of a library that is
 * not here: the whitespace of a word boundary is the six bytes of C.
 */
#include "ulibc.h"

#include "kernel/errno.h"

#define WC_CHUNK 512
#define WC_SHOW_LINES 1u
#define WC_SHOW_WORDS 2u
#define WC_SHOW_BYTES 4u

struct wc_counts {
    unsigned long lines;
    unsigned long words;
    unsigned long bytes;
};

static int wc_space(char byte)
{
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' ||
           byte == '\f';
}

static int count_fd(int fd, struct wc_counts *counts)
{
    char buffer[WC_CHUNK];
    int in_word = 0;

    for (;;) {
        long got = read(fd, buffer, sizeof(buffer));
        long i;

        if (got < 0) {
            if (got == KERR_AGAIN) {
                continue;
            }
            return -1;
        }

        if (got == 0) {
            return 0;
        }

        counts->bytes += (unsigned long)got;

        for (i = 0; i < got; i++) {
            if (buffer[i] == '\n') {
                counts->lines++;
            }

            if (wc_space(buffer[i])) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                counts->words++;
            }
        }
    }
}

static void print_counts(const struct wc_counts *counts, unsigned flags, const char *name)
{
    int first = 1;

    if ((flags & WC_SHOW_LINES) != 0) {
        printf("%lu", counts->lines);
        first = 0;
    }
    if ((flags & WC_SHOW_WORDS) != 0) {
        printf(first ? "%lu" : " %lu", counts->words);
        first = 0;
    }
    if ((flags & WC_SHOW_BYTES) != 0) {
        printf(first ? "%lu" : " %lu", counts->bytes);
    }

    if (name != NULL) {
        printf(" %s", name);
    }

    printf("\n");
}

int main(int argc, char *const argv[], char *const envp[])
{
    unsigned flags = 0;
    int status = ULIBC_EXIT_SUCCESS;
    int first = 1;
    int i;

    (void)envp;

    /* The flags of the line: -l, -w, -c, alone or together. */
    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *letter = argv[first] + 1;

        for (; *letter != '\0'; letter++) {
            if (*letter == 'l') {
                flags |= WC_SHOW_LINES;
            } else if (*letter == 'w') {
                flags |= WC_SHOW_WORDS;
            } else if (*letter == 'c') {
                flags |= WC_SHOW_BYTES;
            } else {
                printf("wc: -%c: unknown option\n", *letter);
                return ULIBC_EXIT_FAILURE;
            }
        }

        first++;
    }

    if (flags == 0) {
        flags = WC_SHOW_LINES | WC_SHOW_WORDS | WC_SHOW_BYTES;
    }

    if (first >= argc) {
        struct wc_counts counts = {0, 0, 0};

        if (count_fd(ULIBC_STDIN, &counts) != 0) {
            printf("wc: (stdin): cannot read\n");
            return ULIBC_EXIT_FAILURE;
        }

        print_counts(&counts, flags, NULL);

        return ULIBC_EXIT_SUCCESS;
    }

    for (i = first; i < argc; i++) {
        struct wc_counts counts = {0, 0, 0};
        int fd = ULIBC_STDIN;
        int rc;

        if (strcmp(argv[i], "-") != 0) {
            fd = open(argv[i], ULIBC_O_RDONLY);
            if (fd < 0) {
                printf("wc: %s: cannot open\n", argv[i]);
                status = ULIBC_EXIT_FAILURE;
                continue;
            }
        }

        rc = count_fd(fd, &counts);

        if (fd != ULIBC_STDIN) {
            (void)close(fd);
        }

        if (rc != 0) {
            printf("wc: %s: cannot read\n", argv[i]);
            status = ULIBC_EXIT_FAILURE;
            continue;
        }

        print_counts(&counts, flags, strcmp(argv[i], "-") == 0 ? NULL : argv[i]);
    }

    return status;
}
