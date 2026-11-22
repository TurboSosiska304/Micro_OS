/*
 * Micro OS — zcat, the first program of the system built on somebody else's
 * library.
 *
 * The program is the proof of the port of zlib: the .gz is a file of MicroFS
 * opened through zlib's own gz* layer, the bytes are inflated by the library
 * of the guest (not a copy of the host zlib), and the result goes to the
 * console or to a file of the VFS. The decompression happens in the window of
 * the program, with the arena of `components/libc/user`; the kernel is not
 * asked for a block and no byte of glibc is in the image.
 *
 *     zcat [file ...]        write the inflated bytes to standard output
 *     zcat -t [file ...]     check the stream and write nothing
 *     zcat -o FILE [file ...] write to FILE instead of the console
 *
 * No file is standard input, which is descriptor 0 of the task: a redirect
 * or a pipe of the shell fills it, and the port layer waits out the EAGAIN
 * of a pipe whose producer is still there.
 */
#include "ulibc.h"

#include <errno.h>
#include <zlib.h>

#include "kernel/errno.h"

#define ZCAT_CHUNK 8192

static const char g_usage[] = "usage: zcat [-t] [-v] [-o FILE] [FILE ...]\n";

/*
 * The bytes of one stream: gzread hands out pieces of what the library
 * inflated, and write_all waits out a pipe that has no room yet. A write
 * error is the failure of the whole stream, because a short copy would leave
 * a file that looks complete and is not.
 */
static int write_all(int fd, const char *data, size_t count)
{
    while (count > 0) {
        long wrote = write(fd, data, count);

        if (wrote < 0) {
            if (wrote == KERR_AGAIN) {
                continue;
            }
            return (int)wrote;
        }

        if (wrote == 0) {
            return KERR_IO;
        }

        data += wrote;
        count -= (size_t)wrote;
    }

    return 0;
}

static int copy_stream(gzFile stream, int output, FILE *file, const char *name, int verbose)
{
    char buffer[ZCAT_CHUNK];
    unsigned long long total = 0;

    for (;;) {
        int got = gzread(stream, buffer, (unsigned)sizeof(buffer));
        int code = 0;

        if (got < 0) {
            printf("zcat: %s: %s\n", name, gzerror(stream, &code));
            return 1;
        }

        if (got == 0) {
            if (verbose) {
                /* The count is a long long so that the formatter of the
                 * program is checked with the length SQLite and Lua use; the
                 * line goes to the error descriptor, not into the data. */
                char line[96];
                int length = snprintf(line, sizeof(line), "zcat: %s: %llu bytes\n", name, total);

                if (length > 0) {
                    (void)write(ULIBC_STDERR, line, (size_t)length);
                }
            }
            return 0;
        }

        total += (unsigned long long)got;

        if (file != NULL) {
            /* A file of `-o` goes through the streams of the library: the
             * binary mode is the only mode of this system. */
            if (fwrite(buffer, 1, (size_t)got, file) != (size_t)got) {
                printf("zcat: %s: write: %s\n", name, strerror(errno));
                return 1;
            }
        } else if (output >= 0) {
            int rc = write_all(output, buffer, (size_t)got);

            if (rc < 0) {
                printf("zcat: %s: write: %s\n", name, strerror(-rc));
                return 1;
            }
        }
    }
}

static int copy_file(const char *path, int output, FILE *file, int test_only, int verbose)
{
    gzFile stream = gzopen(path, "rb");
    int status;

    if (stream == NULL) {
        printf("zcat: %s: %s\n", path, strerror(errno));
        return 1;
    }

    status = copy_stream(stream, test_only ? -1 : output, test_only ? NULL : file, path, verbose);

    if (gzclose(stream) != Z_OK) {
        printf("zcat: %s: the stream is damaged\n", path);
        status = 1;
    }

    return status;
}

static int copy_stdin(int output, FILE *file, int test_only, int verbose)
{
    gzFile stream = gzdopen(ULIBC_STDIN, "rb");
    int status;

    if (stream == NULL) {
        printf("zcat: (stdin): %s\n", strerror(errno));
        return 1;
    }

    status =
        copy_stream(stream, test_only ? -1 : output, test_only ? NULL : file, "(stdin)", verbose);

    if (gzclose(stream) != Z_OK) {
        printf("zcat: (stdin): the stream is damaged\n");
        status = 1;
    }

    return status;
}

int main(int argc, char *const argv[], char *const envp[])
{
    const char *output_path = NULL;
    FILE *output_file = NULL;
    int test_only = 0;
    int verbose = 0;
    int output = ULIBC_STDOUT;
    int status = 0;
    int files = 0;
    int index;

    (void)envp;

    for (index = 1; index < argc; index++) {
        const char *word = argv[index];

        if (strcmp(word, "-t") == 0 || strcmp(word, "--test") == 0) {
            test_only = 1;
        } else if (strcmp(word, "-v") == 0 || strcmp(word, "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(word, "-o") == 0) {
            index++;
            if (index >= argc) {
                (void)write(ULIBC_STDERR, g_usage, strlen(g_usage));
                return ULIBC_EXIT_FAILURE;
            }
            output_path = argv[index];
        } else if (strcmp(word, "-h") == 0 || strcmp(word, "--help") == 0) {
            (void)write(ULIBC_STDOUT, g_usage, strlen(g_usage));
            return ULIBC_EXIT_SUCCESS;
        } else if (word[0] == '-' && word[1] != '\0') {
            (void)write(ULIBC_STDERR, g_usage, strlen(g_usage));
            return ULIBC_EXIT_FAILURE;
        }
    }

    if (output_path != NULL) {
        output_file = fopen(output_path, "wb");
        if (output_file == NULL) {
            printf("zcat: %s: %s\n", output_path, strerror(errno));
            return ULIBC_EXIT_FAILURE;
        }
        output = -1;
    }

    for (index = 1; index < argc; index++) {
        const char *word = argv[index];

        if (strcmp(word, "-t") == 0 || strcmp(word, "--test") == 0 || strcmp(word, "-v") == 0 ||
            strcmp(word, "--verbose") == 0 || strcmp(word, "-h") == 0 ||
            strcmp(word, "--help") == 0) {
            continue;
        }

        if (strcmp(word, "-o") == 0) {
            index++; /* the name of the output was read above */
            continue;
        }

        files++;
        if (copy_file(word, output, output_file, test_only, verbose) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    if (files == 0) {
        if (copy_stdin(output, output_file, test_only, verbose) != 0) {
            status = ULIBC_EXIT_FAILURE;
        }
    }

    if (output_file != NULL) {
        if (fclose(output_file) != 0) {
            printf("zcat: %s: close: %s\n", output_path, strerror(errno));
            status = ULIBC_EXIT_FAILURE;
        }
    }

    return status;
}
