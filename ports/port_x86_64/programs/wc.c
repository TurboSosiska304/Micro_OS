/*
 * Micro OS — wc, the counts of a file or of the input.
 *
 * The program reads one source in pieces of 512 and counts the newlines and
 * the bytes of it. The options are the two counts a shell asks for most:
 * `-l` is the lines and `-c` the bytes; with neither the program prints both,
 * a line and a byte count. Every name is its own count, and the name `-` (or
 * no name at all) is descriptor 0, the input of the program.
 *
 * A source that cannot be read is a sentence and a status of 1, and the next
 * source is still counted: a filter of a line of files does not stop at the
 * first one that is not there.
 */
#include "ulibc.h"

#define WC_CHUNK 512

struct wc_count {
    unsigned long lines;
    unsigned long bytes;
};

static int wc_stream(int fd, const char *name, int show_lines, int show_bytes)
{
    char buffer[WC_CHUNK];
    struct wc_count count = {0, 0};

    for (;;) {
        long got = read(fd, buffer, sizeof(buffer));
        long i;

        if (got < 0) {
            printf("wc: %s: error %ld\n", name, -got);
            return 1;
        }

        if (got == 0) {
            break;
        }

        for (i = 0; i < got; i++) {
            if (buffer[i] == '\n') {
                count.lines++;
            }
        }
        count.bytes += (unsigned long)got;
    }

    if (show_lines && show_bytes) {
        printf("%lu %lu %s\n", count.lines, count.bytes, name);
    } else if (show_bytes) {
        printf("%lu %s\n", count.bytes, name);
    } else {
        printf("%lu %s\n", count.lines, name);
    }

    return 0;
}

static int wc_one(const char *path, int show_lines, int show_bytes)
{
    int status;
    int fd;

    if (strcmp(path, "-") == 0) {
        return wc_stream(ULIBC_STDIN, "stdin", show_lines, show_bytes);
    }

    fd = open(path, ULIBC_O_RDONLY);
    if (fd < 0) {
        printf("wc: %s: error %d\n", path, -fd);
        return 1;
    }

    status = wc_stream(fd, path, show_lines, show_bytes);
    (void)close(fd);

    return status;
}

int main(int argc, char *const argv[], char *const envp[])
{
    int want_lines = 0;
    int want_bytes = 0;
    int show_lines;
    int show_bytes;
    int status = 0;
    int first = 1;
    int i;

    (void)envp;

    /* The flags of the program: one letter each, and a word may carry both
     * of them, as `-lc` does. */
    while (first < argc && argv[first][0] == '-' && argv[first][1] != '\0') {
        const char *letter = argv[first] + 1;

        for (; *letter != '\0'; letter++) {
            if (*letter == 'l') {
                want_lines = 1;
            } else if (*letter == 'c') {
                want_bytes = 1;
            } else {
                printf("usage: wc [-l] [-c] [file]...\n");
                return ULIBC_EXIT_FAILURE;
            }
        }
        first++;
    }

    show_lines = want_lines || !want_bytes;
    show_bytes = want_bytes || !want_lines;

    if (first == argc) {
        return wc_stream(ULIBC_STDIN, "stdin", show_lines, show_bytes);
    }

    for (i = first; i < argc; i++) {
        if (wc_one(argv[i], show_lines, show_bytes) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
