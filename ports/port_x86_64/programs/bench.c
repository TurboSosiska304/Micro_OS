/*
 * Micro OS — the numbers of the machine in one run of fixed sizes.
 *
 * A release is compared with the one before it by a table of numbers and not
 * by an impression, and the table has to be the same table every time: the
 * counts, the blocks and the sizes below are constants of this program, the
 * tool passes them once, and `tools/x86_bench.py` runs this program on one
 * boot and reads the lines back.
 *
 *     bench syscall <n>           n calls of getpid(), the rate of the door
 *     bench fork <n>              n of fork + execve /bin/echo + waitpid
 *     bench pipe <bytes>          bytes through a pipe, one end to the other
 *     bench filewrite <path> <MiB>    a file of 16 MiB in blocks of 64 KiB
 *     bench fileread <path> <MiB>     the same file read back
 *     bench fill <path> <MiB>         the file of the pipe test, untimed
 *     bench console <lines>       lines written to the console of the shell
 *
 * Every result is one line of the machine-readable table
 *
 *     bench: <name> <value> <unit>
 *
 * and one line of prose with the bytes and the milliseconds behind it. The
 * table is the contract: a name, a number with two decimals and a unit that
 * does not change between releases. The prose is for a person reading the
 * console, and the tool ignores it.
 *
 * Two shapes of the kernel are worth knowing before reading a number:
 *
 *   * the pipe holds 256 bytes and answers EAGAIN after a bounded wait
 *     (ADR 0014), so a write or a read of 64 KiB comes back short and the
 *     loop here retries. A release that gives the pipe a real buffer changes
 *     the number and not the loop, which is the point of the measurement.
 *   * the console is a write of the whole line by the program and not a
 *     stream with a buffer: `lines/s` is the cost of the door and the serial
 *     chip, not of a `printf` of the library.
 *
 * The rates are computed in whole numbers: a program of this port has no
 * floating point (no XMM state is saved at a switch, ADR 0010), so a rate is
 * hundredths of a unit and the table prints the point itself.
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

/* One call of the kernel moves at most `KSYS_MAX_IO` bytes and the pipe of
 * the kernel holds far less: the chunk is the block the loop hands to the
 * call and not the size the call has to move at once. */
#define CHUNK_BYTES 65536u
#define MIB_BYTES (1024ull * 1024ull)

/* The console line of the test: long enough to be a line of a shell, short
 * enough that the number is the line and not the bytes. */
#define CONSOLE_LINE "bench console: the line of the measurement\n"

static unsigned char g_chunk[CHUNK_BYTES];

/* The pattern of a chunk: the offset decides the byte, so a read of the wrong
 * block of the volume does not agree with the write by luck. */
static void chunk_fill(unsigned char *buffer, size_t size)
{
    size_t i;

    for (i = 0; i < size; i++) {
        buffer[i] = (unsigned char)((i * 31u + 7u) & 0xFFu);
    }
}

/* The milliseconds between two moments of the one clock of the machine. */
static unsigned long long elapsed_ms(const struct timespec *from, const struct timespec *to)
{
    long long seconds = (long long)(to->tv_sec - from->tv_sec);
    long long nanos = (long long)(to->tv_nsec - from->tv_nsec);
    long long ms = seconds * 1000 + nanos / 1000000;

    return ms > 0 ? (unsigned long long)ms : 0;
}

/* One row of the table: hundredths of a unit, and the point printed here so
 * that the tool parses a number and not two integers. */
static void report(const char *name, unsigned long long hundredths, const char *unit)
{
    printf("bench: %s %llu.%02llu %s\n", name, hundredths / 100ull, hundredths % 100ull, unit);
}

static void note(const char *name, const char *what, unsigned long long count,
                 unsigned long long ms)
{
    printf("bench note: %s %llu %s in %llu ms\n", name, count, what, ms);
}

/* A count of things in a second, in hundredths. A run that finished inside a
 * millisecond is not a number the clock of this machine can divide by; the
 * loop counts enough operations that it does not happen, and a zero is
 * reported as the floor it is. */
static unsigned long long per_second(unsigned long long count, unsigned long long ms)
{
    if (ms == 0) {
        ms = 1;
    }

    return count * 100000ull / ms;
}

/* The same for a rate of bytes: MiB in a second, in hundredths. */
static unsigned long long mib_per_second(unsigned long long bytes, unsigned long long ms)
{
    if (ms == 0) {
        ms = 1;
    }

    return bytes * 100000ull / (ms * MIB_BYTES);
}

static int bench_syscall(unsigned long calls)
{
    struct timespec start;
    struct timespec stop;
    unsigned long long ms;
    long pid = 0;
    unsigned long i;

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    for (i = 0; i < calls; i++) {
        pid = getpid();
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    ms = elapsed_ms(&start, &stop);

    if (pid <= 0) {
        printf("bench error: getpid answered %ld\n", pid);
        return 1;
    }

    note("syscall_getpid", "calls", calls, ms);
    report("syscall_getpid", per_second(calls, ms), "calls/s");

    return 0;
}

/* The lines of the child of the test: a name, a word and the end. The child
 * writes them nowhere because its stdout is /dev/null; the cost of the write
 * is the cost of `echo` and not of the console. */
static char *const g_echo_argv[] = {"echo", "bench", NULL};
static char *const g_echo_envp[] = {"PATH=/bin", NULL};

static int bench_fork(unsigned long runs)
{
    struct timespec start;
    struct timespec stop;
    unsigned long long ms;
    /* The counter is volatile because the compiler is right: fork() returns
     * twice, and a register of the parent is not a place to keep a count
     * across the call. */
    volatile unsigned long done = 0;
    unsigned long i;
    int devnull;
    int status;

    devnull = open("/dev/null", O_WRONLY);

    if (devnull < 0) {
        printf("bench error: /dev/null: %s\n", strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    for (i = 0; i < runs; i++) {
        long pid = fork();

        if (pid == 0) {
            /* The child is the image of this program until execve replaces
             * it: the dup2 and the close are the two calls it makes and the
             * program it becomes never sees them. */
            if (dup2(devnull, ULIBC_STDOUT) < 0) {
                exit(125);
            }

            (void)close(devnull);
            (void)execve("/bin/echo", g_echo_argv, g_echo_envp);
            exit(127);
        }

        if (pid < 0) {
            printf("bench error: fork: %s\n", strerror(errno));
            (void)close(devnull);
            return 1;
        }

        if (waitpid((pid_t)pid, &status, 0) != (pid_t)pid) {
            printf("bench error: waitpid: %s\n", strerror(errno));
            (void)close(devnull);
            return 1;
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("bench error: the child ended with %d\n", status);
            (void)close(devnull);
            return 1;
        }

        done++;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    ms = elapsed_ms(&start, &stop);
    (void)close(devnull);

    note("fork_exec", "runs", done, ms);
    report("fork_exec", per_second(done, ms), "runs/s");

    return 0;
}

/* The writer of the pipe: the bytes in blocks of the chunk, and a retry when
 * the pipe is full and the kernel answered EAGAIN (ADR 0014). It ends the
 * task through exit() and not through a return: the parent owns the loop. */
static void pipe_writer(int fd, unsigned long long bytes)
{
    unsigned long long put = 0;

    while (put < bytes) {
        size_t want = CHUNK_BYTES;
        long wrote;

        if ((unsigned long long)want > bytes - put) {
            want = (size_t)(bytes - put);
        }

        wrote = write(fd, g_chunk, want);

        if (wrote < 0) {
            if (errno == EAGAIN) {
                continue;
            }

            exit(1);
        }

        put += (unsigned long long)wrote;
    }

    (void)close(fd);
    exit(0);
}

static int bench_pipe(unsigned long long bytes)
{
    struct timespec start;
    struct timespec stop;
    unsigned long long ms;
    /* The count of the bytes read lives across the fork, which returns twice. */
    volatile unsigned long long got = 0;
    int fds[2];
    int status;
    long pid;

    if (pipe(fds) != 0) {
        printf("bench error: pipe: %s\n", strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    pid = fork();

    if (pid == 0) {
        (void)close(fds[0]);
        pipe_writer(fds[1], bytes);
        /* The writer ends through exit() and the compiler cannot see the
         * attribute across the call: a return that never happens, written
         * so that the child can never fall into the loop of the parent. */
        exit(1);
    }

    if (pid < 0) {
        printf("bench error: fork: %s\n", strerror(errno));
        (void)close(fds[0]);
        (void)close(fds[1]);
        return 1;
    }

    (void)close(fds[1]);

    while (got < bytes) {
        size_t want = CHUNK_BYTES;
        long read_bytes;

        if ((unsigned long long)want > bytes - got) {
            want = (size_t)(bytes - got);
        }

        read_bytes = read(fds[0], g_chunk, want);

        if (read_bytes < 0) {
            if (errno == EAGAIN) {
                continue;
            }

            printf("bench error: read of the pipe: %s\n", strerror(errno));
            (void)close(fds[0]);
            return 1;
        }

        if (read_bytes == 0) {
            break;
        }

        got += (unsigned long long)read_bytes;
    }

    (void)close(fds[0]);

    if (waitpid((pid_t)pid, &status, 0) != (pid_t)pid) {
        printf("bench error: waitpid: %s\n", strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    ms = elapsed_ms(&start, &stop);

    if (got != bytes) {
        printf("bench error: the pipe carried %llu of %llu bytes\n", got, bytes);
        return 1;
    }

    note("pipe", "bytes", bytes, ms);
    report("pipe", mib_per_second(bytes, ms), "MiB/s");

    return 0;
}

static int bench_filewrite(const char *path, unsigned long mib)
{
    unsigned long long bytes = (unsigned long long)mib * MIB_BYTES;
    unsigned long long done = 0;
    struct timespec start;
    struct timespec stop;
    unsigned long long ms;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0) {
        printf("bench error: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    while (done < bytes) {
        size_t want = CHUNK_BYTES;
        long wrote;

        if ((unsigned long long)want > bytes - done) {
            want = (size_t)(bytes - done);
        }

        wrote = write(fd, g_chunk, want);

        if (wrote != (long)want) {
            printf("bench error: %s: %s\n", path, wrote < 0 ? strerror(errno) : "short write");
            (void)close(fd);
            return 1;
        }

        done += (unsigned long long)want;
    }

    /* The close is inside the measurement: the metadata of the file and the
     * last block reach the volume through it, and a program of a person pays
     * the same call. `mkbig` of `make x86-volume-smoke` times it that way, so
     * the two tools are compared on one rule. */
    if (close(fd) != 0) {
        printf("bench error: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    ms = elapsed_ms(&start, &stop);

    note("file_write", "bytes", bytes, ms);
    report("file_write", mib_per_second(bytes, ms), "MiB/s");

    return 0;
}

static int bench_fileread(const char *path, unsigned long mib)
{
    unsigned long long bytes = (unsigned long long)mib * MIB_BYTES;
    unsigned long long done = 0;
    struct timespec start;
    struct timespec stop;
    unsigned long long ms;
    int fd;

    fd = open(path, O_RDONLY);

    if (fd < 0) {
        printf("bench error: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    while (done < bytes) {
        size_t want = CHUNK_BYTES;
        long read_bytes;

        if ((unsigned long long)want > bytes - done) {
            want = (size_t)(bytes - done);
        }

        read_bytes = read(fd, g_chunk, want);

        if (read_bytes != (long)want) {
            printf("bench error: %s: %s\n", path, read_bytes < 0 ? strerror(errno) : "short read");
            (void)close(fd);
            return 1;
        }

        done += (unsigned long long)want;
    }

    if (close(fd) != 0) {
        printf("bench error: %s: %s\n", path, strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    ms = elapsed_ms(&start, &stop);

    note("file_read", "bytes", bytes, ms);
    report("file_read", mib_per_second(bytes, ms), "MiB/s");

    return 0;
}

/* The file of the pipe test of the shell (`cat file | wc -c`): written with
 * the pattern of the chunk and not timed, because the measurement of that
 * line is the wall clock of the tool and not this program. */
static int bench_fill(const char *path, unsigned long mib)
{
    unsigned long long bytes = (unsigned long long)mib * MIB_BYTES;
    unsigned long long done = 0;
    int fd;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0) {
        printf("bench error: %s: %s\n", path, strerror(errno));
        return 1;
    }

    while (done < bytes) {
        size_t want = CHUNK_BYTES;
        long wrote;

        if ((unsigned long long)want > bytes - done) {
            want = (size_t)(bytes - done);
        }

        wrote = write(fd, g_chunk, want);

        if (wrote != (long)want) {
            printf("bench error: %s: %s\n", path, wrote < 0 ? strerror(errno) : "short write");
            (void)close(fd);
            return 1;
        }

        done += (unsigned long long)want;
    }

    if (close(fd) != 0) {
        printf("bench error: %s: %s\n", path, strerror(errno));
        return 1;
    }

    printf("bench note: fill %llu bytes\n", bytes);

    return 0;
}

static int bench_console(unsigned long lines)
{
    struct timespec start;
    struct timespec stop;
    unsigned long long ms;
    unsigned long i;

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    for (i = 0; i < lines; i++) {
        if (write(ULIBC_STDOUT, CONSOLE_LINE, sizeof(CONSOLE_LINE) - 1) !=
            (long)(sizeof(CONSOLE_LINE) - 1)) {
            printf("bench error: the console: %s\n", strerror(errno));
            return 1;
        }
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    ms = elapsed_ms(&start, &stop);

    note("console", "lines", lines, ms);
    report("console", per_second(lines, ms), "lines/s");

    return 0;
}

static unsigned long parse_count(const char *text, int *ok)
{
    char *end = NULL;
    unsigned long value;

    *ok = 0;
    errno = 0;
    value = strtoul(text, &end, 10);

    if (errno != 0 || end == NULL || *end != '\0' || value == 0u) {
        return 0;
    }

    *ok = 1;

    return value;
}

static int usage(void)
{
    static const char text[] =
        "usage: bench syscall <calls> | fork <runs> | pipe <bytes> |\n"
        "             filewrite|fileread|fill <path> <MiB> | console <lines>\n";

    (void)write(ULIBC_STDERR, text, sizeof(text) - 1);

    return ULIBC_EXIT_FAILURE;
}

int main(int argc, char *const argv[], char *const envp[])
{
    unsigned long value;
    unsigned long mib;
    int ok;

    (void)envp;

    /* The table and the line of the test share the console: a buffered line
     * would be written after the 200 lines of `console` and the count of the
     * stream would be a second measurement. */
    setvbuf(stdout, NULL, _IONBF, 0);
    chunk_fill(g_chunk, CHUNK_BYTES);

    if (argc < 3) {
        return usage();
    }

    if (strcmp(argv[1], "filewrite") == 0 || strcmp(argv[1], "fileread") == 0 ||
        strcmp(argv[1], "fill") == 0) {
        if (argc != 4) {
            return usage();
        }

        mib = parse_count(argv[3], &ok);

        if (!ok) {
            printf("bench error: %s: not a size of megabytes\n", argv[3]);
            return ULIBC_EXIT_FAILURE;
        }

        if (strcmp(argv[1], "filewrite") == 0) {
            return bench_filewrite(argv[2], mib);
        }

        if (strcmp(argv[1], "fileread") == 0) {
            return bench_fileread(argv[2], mib);
        }

        return bench_fill(argv[2], mib);
    }

    if (argc != 3) {
        return usage();
    }

    value = parse_count(argv[2], &ok);

    if (!ok) {
        printf("bench error: %s: not a count\n", argv[2]);
        return ULIBC_EXIT_FAILURE;
    }

    if (strcmp(argv[1], "syscall") == 0) {
        return bench_syscall(value);
    }

    if (strcmp(argv[1], "fork") == 0) {
        return bench_fork(value);
    }

    if (strcmp(argv[1], "pipe") == 0) {
        return bench_pipe((unsigned long long)value);
    }

    if (strcmp(argv[1], "console") == 0) {
        return bench_console(value);
    }

    printf("bench error: %s: not a mode\n", argv[1]);

    return ULIBC_EXIT_FAILURE;
}
