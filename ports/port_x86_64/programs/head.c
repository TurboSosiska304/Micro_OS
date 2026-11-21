/*
 * Micro OS — head, the first lines of a file or of the input.
 *
 * The program copies the bytes of a source to the console and stops after
 * line N: `-n N` says how many, ten when the line says nothing. A block of
 * the source is written as far as the last newline that still belongs to the
 * count, so the call of the console is one per block and not one per byte.
 *
 * A file is read by its path; no name at all, or `-`, is descriptor 0, the
 * input of the program. A source that cannot be read is a sentence and a
 * status of 1.
 */
#include "ulibc.h"

#include "kernel/errno.h"

#define HEAD_CHUNK 512

static int head_stream(int fd, const char *name, unsigned long want)
{
    char buffer[HEAD_CHUNK];
    unsigned long done = 0;

    if (want == 0) {
        return 0; /* -n 0 is no line at all, not an error */
    }

    for (;;) {
        long got = read(fd, buffer, sizeof(buffer));
        long take = -1;
        long i;
        int finished = 0;

        if (got < 0) {
            printf("head: %s: error %ld\n", name, -got);
            return 1;
        }

        if (got == 0) {
            break;
        }

        for (i = 0; i < got; i++) {
            if (buffer[i] != '\n') {
                continue;
            }
            if (done + 1 == want) {
                take = i + 1;
                finished = 1;
                break;
            }
            done++;
        }

        if (take < 0) {
            take = got; /* the block holds no last line yet */
        }

        if (write(ULIBC_STDOUT, buffer, (size_t)take) != take) {
            printf("head: %s: error %d\n", name, KERR_IO);
            return 1;
        }

        if (finished) {
            break; /* the line N was the end of this block */
        }
    }

    return 0;
}

static int head_one(const char *path, unsigned long want)
{
    int status;
    int fd;

    if (strcmp(path, "-") == 0) {
        return head_stream(ULIBC_STDIN, "stdin", want);
    }

    fd = open(path, ULIBC_O_RDONLY);
    if (fd < 0) {
        printf("head: %s: error %d\n", path, -fd);
        return 1;
    }

    status = head_stream(fd, path, want);
    (void)close(fd);

    return status;
}

/* The decimal number of the option, or -1 when the word is not one. */
static long parse_count(const char *text)
{
    unsigned long value = 0;

    if (*text == '\0') {
        return -1;
    }

    for (; *text != '\0'; text++) {
        if (*text < '0' || *text > '9') {
            return -1;
        }
        if (value > 100000000UL) {
            return -1; /* a number far larger than any line count */
        }
        value = value * 10UL + (unsigned long)(*text - '0');
    }

    return (long)value;
}

int main(int argc, char *const argv[], char *const envp[])
{
    unsigned long want = 10;
    int first = 1;
    int status = 0;
    int i;

    (void)envp;

    if (first + 1 < argc && strcmp(argv[first], "-n") == 0) {
        long count = parse_count(argv[first + 1]);

        if (count < 0) {
            printf("usage: head [-n N] [file]\n");
            return ULIBC_EXIT_FAILURE;
        }
        want = (unsigned long)count;
        first += 2;
    }

    if (first == argc) {
        return head_stream(ULIBC_STDIN, "stdin", want);
    }

    for (i = first; i < argc; i++) {
        if (head_one(argv[i], want) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
