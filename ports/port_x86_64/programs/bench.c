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
 *     bench latsyscall <samples>  the p50 and p99 of one call of getpid()
 *     bench latpipe <samples>     the same of one byte through a pipe
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
 *
 * The two latency rows are the exception to the rates: the clock of the
 * machine has a millisecond, so one call of a microsecond is timed by the
 * time stamp counter of the processor around the call itself and the samples
 * are counted into a histogram. The counter is calibrated against the
 * millisecond clock at the start of the measurement; its two reads are a
 * floor of every sample and the floor is measured and printed beside it.
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

/* --- the latency of one operation ---------------------------------------- */

/*
 * The counter of the processor. The clock of the machine ticks a millisecond,
 * which is a thousand calls of the door, so a latency is measured by the two
 * reads of the time stamp counter around the call. LFENCE keeps the reads
 * from moving across the instruction that is measured (the kernel keeps its
 * own copy of this in port_x86_64.h, which a program does not include).
 */
static unsigned long long tsc_now(void)
{
    unsigned int low;
    unsigned int high;

    __asm__ volatile("lfence\n\trdtsc" : "=a"(low), "=d"(high) : : "memory");

    return ((unsigned long long)high << 32) | low;
}

/* The ticks of the counter in one microsecond: the counter is calibrated
 * against the one clock of the machine, over a window long enough that the
 * millisecond it ticks in does not matter. */
#define CALIBRATION_MS 100u

static unsigned long g_tsc_per_us;

static int tsc_calibrate(void)
{
    struct timespec start;
    struct timespec now;
    unsigned long long first;
    unsigned long long last;
    unsigned long long us;

    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    first = tsc_now();

    do {
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
    } while (elapsed_ms(&start, &now) < CALIBRATION_MS);

    last = tsc_now();
    us = elapsed_ms(&start, &now) * 1000ull;

    if (us == 0) {
        return 0;
    }

    g_tsc_per_us = (unsigned long)((last - first) / us);

    return g_tsc_per_us != 0;
}

static unsigned long long tsc_to_ns(unsigned long long ticks)
{
    return ticks * 1000ull / (unsigned long long)g_tsc_per_us;
}

/*
 * The histogram of one measurement: the count of the samples that fell into
 * each bucket of `bucket_ns`, and one bucket more than the range for the
 * samples beyond it. The percentiles are read from the counts and not from an
 * array of the samples, so a measurement of any length costs the same memory;
 * the middle of a bucket is the value it reports, which is the resolution of
 * the row.
 */
struct latency {
    unsigned int *buckets;
    unsigned long count;
    unsigned long bucket_ns;
    unsigned long samples;
};

static void latency_reset(struct latency *lat, unsigned int *buckets, unsigned long count,
                          unsigned long bucket_ns)
{
    unsigned long i;

    lat->buckets = buckets;
    lat->count = count;
    lat->bucket_ns = bucket_ns;
    lat->samples = 0;

    /* The bucket of the samples beyond the range is part of the array. */
    for (i = 0; i <= count; i++) {
        buckets[i] = 0;
    }
}

static void latency_add(struct latency *lat, unsigned long long ns)
{
    unsigned long index = (unsigned long)(ns / lat->bucket_ns);

    if (index >= lat->count) {
        index = lat->count;
    }

    lat->buckets[index]++;
    lat->samples++;
}

/* The nearest rank: the value of the sample at the ceil(p % of the count)th
 * place, which is the definition a tail is read by (and not an average of two
 * neighbours, which would smooth the tail this row exists for). */
static unsigned long long latency_percentile(const struct latency *lat, unsigned percent)
{
    unsigned long long rank = (lat->samples * (unsigned long long)percent + 99ull) / 100ull;
    unsigned long long seen = 0;
    unsigned long i;

    for (i = 0; i < lat->count; i++) {
        seen += lat->buckets[i];

        if (seen >= rank) {
            return (unsigned long long)i * lat->bucket_ns + lat->bucket_ns / 2;
        }
    }

    /* Every sample that reached this point is beyond the range: the row is
     * the top of the histogram and the note says how many there were. */
    return (unsigned long long)lat->count * lat->bucket_ns;
}

/* A count of samples that did not fit the range of the histogram, and the
 * top of that range: a row that is a floor of the truth says so. */
static void latency_beyond(const char *name, const struct latency *lat)
{
    unsigned long long beyond = lat->buckets[lat->count];

    if (beyond != 0) {
        printf("bench note: %s_beyond %llu samples beyond %llu ns\n", name, beyond,
               (unsigned long long)lat->count * lat->bucket_ns);
    }
}

static void report_ns(const char *name, unsigned long long ns)
{
    report(name, ns * 100ull, "ns");
}

/*
 * The buckets of the two measurements. The call of the door is under a
 * microsecond and the pipe is the tick of the scheduler, so they need ranges
 * three orders of magnitude apart; both are 4096 buckets of a count each
 * (16 KiB of the data of the program per measurement).
 */
#define LAT_SYSCALL_BUCKET_NS 16u
#define LAT_SYSCALL_BUCKETS 4096u
#define LAT_PIPE_BUCKET_NS 1024u
#define LAT_PIPE_BUCKETS 4096u

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

/* --- the latency of a call and of a byte --------------------------------- */

static int bench_lat_syscall(unsigned long samples)
{
    static unsigned int buckets[LAT_SYSCALL_BUCKETS + 1u];
    /* 512 buckets of the range and the one beyond it. */
    static unsigned int overhead_buckets[512u + 1u];
    struct latency lat;
    struct latency overhead;
    struct timespec start;
    struct timespec stop;
    unsigned long long ms;
    unsigned long i;

    if (!tsc_calibrate()) {
        printf("bench error: the counter of the processor did not advance\n");
        return 1;
    }

    /* The floor of every sample: the same pair of reads around no operation
     * at all, in the same loop. A reader subtracts it from the row. */
    latency_reset(&overhead, overhead_buckets, 512u, 8u);

    for (i = 0; i < samples; i++) {
        unsigned long long first = tsc_now();

        latency_add(&overhead, tsc_to_ns(tsc_now() - first));
    }

    latency_reset(&lat, buckets, LAT_SYSCALL_BUCKETS, LAT_SYSCALL_BUCKET_NS);

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    for (i = 0; i < samples; i++) {
        unsigned long long first = tsc_now();
        long pid = getpid();

        latency_add(&lat, tsc_to_ns(tsc_now() - first));

        if (pid <= 0) {
            printf("bench error: getpid answered %ld\n", pid);
            return 1;
        }
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    ms = elapsed_ms(&start, &stop);

    note("lat_syscall", "samples", samples, ms);
    printf("bench note: lat_syscall_floor %llu ns for the pair of reads of the counter\n",
           latency_percentile(&overhead, 50));
    latency_beyond("lat_syscall", &lat);

    report_ns("lat_syscall_p50", latency_percentile(&lat, 50));
    report_ns("lat_syscall_p99", latency_percentile(&lat, 99));

    return 0;
}

/* A message of the pipe test: the moment of the writer just before the write
 * and the moment of the reader just after the read. The two are the counter
 * of the one processor of the run, so their difference is the age of the byte
 * and no second clock is involved. */
struct pipe_stamp {
    unsigned long long written;
    unsigned long long received;
};

/* The pipe is a stream of the kernel: a read or a write may come back short
 * and may answer EAGAIN when the ring of 256 bytes is full (ADR 0014), so a
 * message of sixteen bytes is handed over by a loop that retries. */
static int pipe_write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t put = 0;

    while (put < size) {
        long wrote = write(fd, bytes + put, size - put);

        if (wrote < 0) {
            if (errno == EAGAIN) {
                continue;
            }

            return -1;
        }

        if (wrote == 0) {
            return -1;
        }

        put += (size_t)wrote;
    }

    return 0;
}

static int pipe_read_all(int fd, void *buffer, size_t size)
{
    unsigned char *bytes = (unsigned char *)buffer;
    size_t got = 0;

    while (got < size) {
        long read_bytes = read(fd, bytes + got, size - got);

        if (read_bytes < 0) {
            if (errno == EAGAIN) {
                continue;
            }

            return -1;
        }

        if (read_bytes == 0) {
            return -1;
        }

        got += (size_t)read_bytes;
    }

    return 0;
}

/* The reader of the pipe test: it stamps the moment the byte is in its hands
 * and sends the stamp back, which is the only thing the writer waits for
 * between two bytes. It ends the task through exit() and never returns. */
static void lat_pipe_reader(int data_fd, int ack_fd, unsigned long samples)
{
    unsigned long i;

    for (i = 0; i < samples; i++) {
        struct pipe_stamp stamp;

        if (pipe_read_all(data_fd, &stamp, sizeof(stamp)) != 0) {
            exit(1);
        }

        stamp.received = tsc_now();

        if (pipe_write_all(ack_fd, &stamp, sizeof(stamp)) != 0) {
            exit(1);
        }
    }

    exit(0);
}

/*
 * The delay of one byte through a pipe: the writer stamps the counter before
 * the write, the reader stamps it after the read and sends the stamp back,
 * and the difference is the time the byte spent in the pipe (the wake of the
 * reader included). One message is in flight at a time and the ack is what
 * keeps it so: the writer waits for the answer before it stamps the next
 * byte, and no sample waits behind another.
 */
static int bench_lat_pipe(unsigned long samples)
{
    static unsigned int buckets[LAT_PIPE_BUCKETS + 1u];
    struct latency lat;
    struct timespec start;
    struct timespec stop;
    unsigned long long ms;
    int data[2];
    int ack[2];
    unsigned long i;
    int status;
    long pid;

    if (pipe(data) != 0 || pipe(ack) != 0) {
        printf("bench error: pipe: %s\n", strerror(errno));
        return 1;
    }

    if (!tsc_calibrate()) {
        printf("bench error: the counter of the processor did not advance\n");
        return 1;
    }

    latency_reset(&lat, buckets, LAT_PIPE_BUCKETS, LAT_PIPE_BUCKET_NS);

    (void)clock_gettime(CLOCK_MONOTONIC, &start);

    pid = fork();

    if (pid == 0) {
        (void)close(data[1]);
        (void)close(ack[0]);
        lat_pipe_reader(data[0], ack[1], samples);
        exit(1);
    }

    if (pid < 0) {
        printf("bench error: fork: %s\n", strerror(errno));
        (void)close(data[0]);
        (void)close(data[1]);
        (void)close(ack[0]);
        (void)close(ack[1]);
        return 1;
    }

    (void)close(data[0]);
    (void)close(ack[1]);

    for (i = 0; i < samples; i++) {
        struct pipe_stamp sent;
        struct pipe_stamp back;
        unsigned long long age;

        sent.written = tsc_now();
        sent.received = 0;

        if (pipe_write_all(data[1], &sent, sizeof(sent)) != 0) {
            printf("bench error: write of the pipe: %s\n", strerror(errno));
            (void)close(data[1]);
            (void)close(ack[0]);
            return 1;
        }

        if (pipe_read_all(ack[0], &back, sizeof(back)) != 0) {
            printf("bench error: read of the pipe: %s\n", strerror(errno));
            (void)close(data[1]);
            (void)close(ack[0]);
            return 1;
        }

        age = (back.received > sent.written) ? back.received - sent.written : 0ull;
        latency_add(&lat, tsc_to_ns(age));
    }

    (void)close(data[1]);
    (void)close(ack[0]);

    if (waitpid((pid_t)pid, &status, 0) != (pid_t)pid) {
        printf("bench error: waitpid: %s\n", strerror(errno));
        return 1;
    }

    (void)clock_gettime(CLOCK_MONOTONIC, &stop);
    ms = elapsed_ms(&start, &stop);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("bench error: the reader ended with %d\n", status);
        return 1;
    }

    if (lat.samples != samples) {
        printf("bench error: the pipe carried %lu of %lu bytes\n", lat.samples, samples);
        return 1;
    }

    note("lat_pipe", "samples", samples, ms);
    latency_beyond("lat_pipe", &lat);

    report_ns("lat_pipe_p50", latency_percentile(&lat, 50));
    report_ns("lat_pipe_p99", latency_percentile(&lat, 99));

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
        "             filewrite|fileread|fill <path> <MiB> | console <lines> |\n"
        "             latsyscall <samples> | latpipe <samples>\n";

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

    if (strcmp(argv[1], "latsyscall") == 0) {
        return bench_lat_syscall(value);
    }

    if (strcmp(argv[1], "latpipe") == 0) {
        return bench_lat_pipe(value);
    }

    printf("bench error: %s: not a mode\n", argv[1]);

    return ULIBC_EXIT_FAILURE;
}
