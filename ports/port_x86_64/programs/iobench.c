/*
 * Micro OS — the grain of the I/O of the machine, measured by the machine.
 *
 * The release moves a file through the calls in blocks of 64 KiB, and the
 * numbers that matter are the calls and the time: a pipe of 256 bytes turns
 * one `write` of 64 KiB into hundreds of waits, and a stream with no buffer
 * turns a line reader into one call per byte. This program is the ruler of
 * both: it copies a file through a pipe of the kernel in a child and counts
 * the calls of each side, reads a file a byte at a time through `fgetc`, and
 * writes a file a line at a time through a stream.
 *
 * It is a program of a test and not of a person (like `mkbig`): the smoke
 * test of the image does not carry it, `tools/io_bench.py` builds a volume
 * with it and reads the lines.
 *
 *     iobench pipe <file> [chunk]  copy the file through a pipe, two tasks
 *     iobench getc <file>          read the file with fgetc, the worst reader
 *     iobench line <file> <lines>  write a file of lines through a stream
 *
 * The rate is whole arithmetic because a program of this machine has no
 * floating point by default; the hundredths of a MiB a second are the two
 * integers of the division.
 */
#include "ulibc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_MAX 65536u
#define MIB_BYTES 1048576u

static unsigned char g_buffer[CHUNK_MAX];

static unsigned long long elapsed_ms(const struct timespec *from, const struct timespec *to)
{
    long long seconds = (long long)(to->tv_sec - from->tv_sec);
    long long nanos = (long long)(to->tv_nsec - from->tv_nsec);
    long long ms = seconds * 1000 + nanos / 1000000;

    return ms > 0 ? (unsigned long long)ms : 0;
}

static unsigned long long hundredths(unsigned long long bytes, unsigned long long ms)
{
    if (ms == 0) {
        return 0;
    }

    return (bytes * 100000ull) / (ms * (unsigned long long)MIB_BYTES);
}

/* The writer of the pipe: the file in blocks, like `cat`. */
static int pipe_writer(const char *path, int out, size_t chunk)
{
    unsigned long long total = 0;
    unsigned long reads = 0;
    unsigned long writes = 0;
    int file = open(path, O_RDONLY);

    if (file < 0) {
        printf("iobench: %s: %s\n", path, strerror(errno));
        return 1;
    }

    for (;;) {
        long got = read(file, g_buffer, chunk);

        if (got < 0) {
            printf("iobench: read: %s\n", strerror(errno));
            close(file);
            close(out);
            return 1;
        }
        if (got == 0) {
            break;
        }
        reads++;
        total += (unsigned long long)got;

        {
            long put = 0;

            while (put < got) {
                long wrote = write(out, g_buffer + put, (unsigned long)(got - put));

                if (wrote < 0) {
                    printf("iobench: write: %s\n", strerror(errno));
                    close(file);
                    close(out);
                    return 1;
                }
                put += wrote;
                writes++;
            }
        }
    }

    close(file);
    close(out);
    printf("iobench: writer %llu bytes in %lu reads, %lu writes\n", total, reads, writes);

    return 0;
}

/* The reader of the pipe: the bytes are counted and dropped, like `wc -c`. */
static int pipe_reader(int in, long pid, size_t chunk)
{
    struct timespec start;
    struct timespec stop;
    unsigned long long total = 0;
    unsigned long reads = 0;
    int status = 0;
    int rc = 0;

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        long got = read(in, g_buffer, sizeof(g_buffer));

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            printf("iobench: read: %s\n", strerror(errno));
            rc = 1;
            break;
        }
        if (got == 0) {
            break;
        }
        total += (unsigned long long)got;
        reads++;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    close(in);
    (void)waitpid(pid, &status, 0);

    printf("iobench: pipe %llu bytes in %lu reads, %llu ms (%llu.%02llu MiB/s), chunk %lu\n", total,
           reads, elapsed_ms(&start, &stop), hundredths(total, elapsed_ms(&start, &stop)) / 100ull,
           hundredths(total, elapsed_ms(&start, &stop)) % 100ull, (unsigned long)chunk);

    return rc;
}

/* Copy the file into the pipe in a child; count the calls of each side. */
static int copy_through_pipe(const char *path, size_t chunk)
{
    int fds[2];
    long pid;

    if (pipe(fds) != 0) {
        printf("iobench: pipe: %s\n", strerror(errno));
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        printf("iobench: fork: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    if (pid == 0) {
        close(fds[0]);
        return pipe_writer(path, fds[1], chunk);
    }

    close(fds[1]);

    return pipe_reader(fds[0], pid, chunk);
}

/* The worst reader of a stream: one function call and one byte at a time. */
static int read_with_getc(const char *path)
{
    struct timespec start;
    struct timespec stop;
    unsigned long long total = 0;
    unsigned long calls = 0;
    FILE *file = fopen(path, "r");
    int byte;

    if (file == NULL) {
        printf("iobench: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    while ((byte = fgetc(file)) != EOF) {
        total++;
        calls++;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    (void)fclose(file);

    printf("iobench: getc %llu bytes in %lu calls, %llu ms\n", total, calls,
           elapsed_ms(&start, &stop));

    return 0;
}

/* A writer of lines through a stream: the shape of a filter of sbase. */
static int write_lines(const char *path, unsigned long lines)
{
    static const char line[] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\n";
    struct timespec start;
    struct timespec stop;
    unsigned long long total = 0;
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        printf("iobench: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    for (unsigned long index = 0; index < lines; index++) {
        if (fwrite(line, 1, sizeof(line) - 1, file) != sizeof(line) - 1) {
            printf("iobench: write: %s\n", strerror(errno));
            (void)fclose(file);
            return 1;
        }
        total += sizeof(line) - 1;
    }
    (void)fclose(file);
    (void)clock_gettime(CLOCK_MONOTONIC, &stop);

    printf("iobench: line %llu bytes in %lu calls, %llu ms\n", total, lines,
           elapsed_ms(&start, &stop));

    return 0;
}

static int usage(void)
{
    printf("usage: iobench pipe <file> [chunk] | getc <file> | line <file> <lines>\n");

    return 2;
}

int main(int argc, char *const argv[], char *const envp[])
{
    (void)envp;

    if (argc < 3) {
        return usage();
    }

    if (strcmp(argv[1], "pipe") == 0) {
        size_t chunk = CHUNK_MAX;
        char *end = NULL;

        if (argc >= 4) {
            unsigned long value = strtoul(argv[3], &end, 10);

            if (end == NULL || *end != '\0' || value == 0 || value > CHUNK_MAX) {
                return usage();
            }
            chunk = (size_t)value;
        }

        return copy_through_pipe(argv[2], chunk);
    }

    if (strcmp(argv[1], "getc") == 0) {
        return read_with_getc(argv[2]);
    }

    if (strcmp(argv[1], "line") == 0) {
        char *end = NULL;
        unsigned long lines = 1000;

        if (argc >= 4) {
            lines = strtoul(argv[3], &end, 10);
            if (end == NULL || *end != '\0') {
                return usage();
            }
        }

        return write_lines(argv[2], lines);
    }

    return usage();
}
