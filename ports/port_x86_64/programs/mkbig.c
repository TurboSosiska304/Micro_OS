/*
 * Micro OS — a file of many megabytes, written and read back by the machine.
 *
 * The smoke test of a large volume needs a file that is large in the way a
 * file of a 5 GiB volume is: it is not the twenty kilobytes of the other
 * tests, which the twelve direct blocks and one table of pointers of ext2
 * hold, but megabytes, which live in the double and the triple table of the
 * format and in a chain of thousands of blocks of MicroFS. The shell has no
 * command that writes a file of a size, so the machine carries a program
 * that does, and the smoke test times it.
 *
 * The pattern of the file is a function of the position, so a read of the
 * wrong block of the volume does not compare equal by luck: every byte of a
 * chunk comes from the index of the chunk and the offset in it.
 *
 *     mkbig write <path> <MiB>    write the file, print the bytes and the rate
 *     mkbig verify <path> <MiB>   read it back, compare, print the rate
 *     mkbig fill <path> <MiB>     the pattern of the two, and no file at all
 *
 * The rate is the time of the call and not of the console: the numbers of
 * `md5sum` of a big file are the cost of the medium, and the tests print the
 * two so that a slow volume is a number and not a feeling.
 *
 * `fill` is the measurement of the machine and not of the volume: it fills the
 * chunks and writes nothing, so the bytes a second of the pattern is the
 * ceiling a file of that size could ever reach, and the difference between it
 * and `write` is the filesystem and the disk (the plan of 1.6 keeps the number
 * of the volume smoke honest with it).
 */
#include "ulibc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* One call of the kernel moves at most `KSYS_MAX_IO` bytes, and the buffer
 * is the chunk of the loop: the program reads and writes in pieces that the
 * call takes, not in one piece of the whole file. */
#define CHUNK_BYTES 65536u
#define MIB_BYTES (1024u * 1024u)

static unsigned char g_chunk[CHUNK_BYTES];

/* The bytes of a chunk: the index of the chunk decides the first byte and
 * the position decides the rest, so the file of a second run of the same
 * size is the same bytes and a block read from the wrong place disagrees. */
static void chunk_fill(unsigned char *buffer, size_t size, unsigned long chunk)
{
    size_t i;

    for (i = 0; i < size; i++) {
        buffer[i] = (unsigned char)((chunk * 31u + (unsigned long)i) & 0xFFu);
    }
}

static void chunk_check(unsigned char *buffer, size_t size, unsigned long chunk, size_t *at)
{
    size_t i;

    for (i = 0; i < size; i++) {
        if (buffer[i] != (unsigned char)((chunk * 31u + (unsigned long)i) & 0xFFu)) {
            *at = i;
            return;
        }
    }

    *at = size;
}

/* The milliseconds between two moments of the monotonic clock. A program
 * of this machine has no floating point (no XMM state is saved, ADR 0010),
 * so the rate is computed in whole numbers: bytes and milliseconds, and a
 * MiB per second in hundredths. */
static unsigned long long elapsed_ms(const struct timespec *from, const struct timespec *to)
{
    long long seconds = (long long)(to->tv_sec - from->tv_sec);
    long long nanos = (long long)(to->tv_nsec - from->tv_nsec);
    long long ms = seconds * 1000 + nanos / 1000000;

    return ms > 0 ? (unsigned long long)ms : 0;
}

static void rate(const char *what, unsigned long long bytes, unsigned long long ms)
{
    unsigned long long hundredths;

    if (ms == 0) {
        printf("mkbig: %s %llu bytes in under a millisecond\n", what, bytes);
        return;
    }

    hundredths = (bytes * 100000ull) / (ms * 1048576ull);
    printf("mkbig: %s %llu bytes in %llu ms (%llu.%02llu MiB/s)\n", what, bytes, ms,
           hundredths / 100ull, hundredths % 100ull);
}

static int write_file(const char *path, unsigned long mib)
{
    unsigned long long bytes = (unsigned long long)mib * MIB_BYTES;
    unsigned long long done = 0;
    unsigned long chunk = 0;
    struct timespec start;
    struct timespec stop;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0) {
        printf("mkbig: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    while (done < bytes) {
        size_t want = CHUNK_BYTES;
        ssize_t wrote;

        if ((unsigned long long)want > bytes - done) {
            want = (size_t)(bytes - done);
        }

        chunk_fill(g_chunk, want, chunk);
        wrote = write(fd, g_chunk, want);

        if (wrote != (ssize_t)want) {
            printf("mkbig: %s: %s\n", path, wrote < 0 ? strerror(errno) : "short write");
            (void)close(fd);
            return 1;
        }

        done += (unsigned long long)want;
        chunk++;
    }

    if (close(fd) != 0) {
        printf("mkbig: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    rate("wrote", bytes, elapsed_ms(&start, &stop));

    return 0;
}

/* TEMP: the cost of the pattern itself, with no file under it. */
static int fill_only(unsigned long mib)
{
    unsigned long long bytes = (unsigned long long)mib * MIB_BYTES;
    unsigned long long done = 0;
    unsigned long chunk = 0;
    struct timespec start;
    struct timespec stop;

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    while (done < bytes) {
        size_t want = CHUNK_BYTES;

        if ((unsigned long long)want > bytes - done) {
            want = (size_t)(bytes - done);
        }

        chunk_fill(g_chunk, want, chunk);
        done += (unsigned long long)want;
        chunk++;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    rate("filled", bytes, elapsed_ms(&start, &stop));

    return 0;
}

static int verify_file(const char *path, unsigned long mib)
{
    unsigned long long bytes = (unsigned long long)mib * MIB_BYTES;
    unsigned long long done = 0;
    unsigned long chunk = 0;
    struct timespec start;
    struct timespec stop;
    int fd;

    fd = open(path, O_RDONLY);

    if (fd < 0) {
        printf("mkbig: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    while (done < bytes) {
        size_t want = CHUNK_BYTES;
        size_t at = 0;
        ssize_t got;

        if ((unsigned long long)want > bytes - done) {
            want = (size_t)(bytes - done);
        }

        got = read(fd, g_chunk, want);

        if (got != (ssize_t)want) {
            printf("mkbig: %s: %s\n", path, got < 0 ? strerror(errno) : "short read");
            (void)close(fd);
            return 1;
        }

        chunk_check(g_chunk, want, chunk, &at);

        if (at != want) {
            printf("mkbig: %s: the byte at %llu is not the one that was written\n", path,
                   done + (unsigned long long)at);
            (void)close(fd);
            return 1;
        }

        done += (unsigned long long)want;
        chunk++;
    }

    /* One byte more than the file is a file that grew: the size of the
     * record and the bytes of the volume have to agree. */
    {
        char extra;

        if (read(fd, &extra, 1) != 0) {
            printf("mkbig: %s: the file is longer than %llu bytes\n", path, bytes);
            (void)close(fd);
            return 1;
        }
    }

    if (close(fd) != 0) {
        printf("mkbig: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    rate("read", bytes, elapsed_ms(&start, &stop));
    printf("mkbig: the bytes agree\n");

    return 0;
}

static unsigned long parse_mib(const char *text, int *ok)
{
    char *end = NULL;
    unsigned long value;

    *ok = 0;
    value = strtoul(text, &end, 10);

    if (end == NULL || *end != '\0' || value == 0u) {
        return 0;
    }

    *ok = 1;

    return value;
}

int main(int argc, char *const argv[], char *const envp[])
{
    unsigned long mib;
    int ok;

    (void)envp;

    if (argc != 4) {
        static const char usage[] = "usage: mkbig write|verify|fill <path> <MiB>\n";

        (void)write(ULIBC_STDERR, usage, sizeof(usage) - 1);
        return ULIBC_EXIT_FAILURE;
    }

    mib = parse_mib(argv[3], &ok);

    if (!ok) {
        printf("mkbig: %s: not a size of megabytes\n", argv[3]);
        return ULIBC_EXIT_FAILURE;
    }

    if (strcmp(argv[1], "write") == 0) {
        return write_file(argv[2], mib);
    }

    if (strcmp(argv[1], "fill") == 0) {
        return fill_only(mib);
    }

    if (strcmp(argv[1], "verify") == 0) {
        return verify_file(argv[2], mib);
    }

    printf("mkbig: %s: not a mode (write, verify or fill)\n", argv[1]);

    return ULIBC_EXIT_FAILURE;
}
