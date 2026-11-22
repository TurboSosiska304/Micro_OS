/*
 * Micro OS — memory test.
 *
 * Shows how much memory is available and how fast it works: the internal
 * heap and the PSRAM are measured the same way, so the difference between
 * the two is visible.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/kmm.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"

/* The size of the test block. */
#define MEMTEST_DEFAULT_SIZE (32 * 1024)
#define MEMTEST_MAX_SIZE (1024 * 1024)
#define MEMTEST_MIN_SIZE 4096

/* The pattern that is written into the block. */
#define MEMTEST_PATTERN 0xA5

/*
 * The shortest interval a speed is taken over. The clock of a port is not
 * finer than its tick (the x86-64 uptime moves by the millisecond), and an
 * operation of a few kilobytes finishes inside one step: the difference
 * would be zero or one step, and a division by one microsecond would claim
 * tens of gigabytes per second. The operation is repeated until the clock
 * has moved, and the count of the runs divides the total.
 */
#define MEMTEST_MIN_US 1000u
#define MEMTEST_MAX_REPS 100000u

struct memtest_result {
    k_size_t total;
    k_size_t free_bytes;
    k_size_t used;
    k_size_t block_size;
    k_u32_t write_kbps;
    k_u32_t read_kbps;
    k_u32_t copy_kbps;
    bool write_measured;
    bool read_measured;
    bool copy_measured;
    bool copy_tested;
    bool checksum_ok;
    bool tested;
};

/* The sum of the block, so that the compiler cannot skip the read. */
static volatile k_u32_t g_sink;

/*
 * The sum of the block, word by word: the compiler turns the copy into a
 * single load. Four independent accumulators keep the processor busy, a
 * single one would measure the latency of the addition instead of the
 * speed of the memory.
 */
static k_u32_t checksum(const uint8_t *data, k_size_t size)
{
    k_u32_t s0 = 0;
    k_u32_t s1 = 0;
    k_u32_t s2 = 0;
    k_u32_t s3 = 0;
    k_size_t i = 0;

    for (; i + 16 <= size; i += 16) {
        k_u32_t w0;
        k_u32_t w1;
        k_u32_t w2;
        k_u32_t w3;

        memcpy(&w0, data + i, sizeof(w0));
        memcpy(&w1, data + i + 4, sizeof(w1));
        memcpy(&w2, data + i + 8, sizeof(w2));
        memcpy(&w3, data + i + 12, sizeof(w3));

        s0 += w0;
        s1 += w1;
        s2 += w2;
        s3 += w3;
    }

    for (; i + 4 <= size; i += 4) {
        k_u32_t word;

        memcpy(&word, data + i, sizeof(word));
        s0 += word;
    }

    s0 += s1 + s2 + s3;

    for (; i < size; i++) {
        s0 += data[i];
    }

    return s0;
}

/* The sum that a block filled with the pattern has to give. */
static k_u32_t expected_checksum(k_size_t size)
{
    k_u32_t sum = 0;
    k_size_t i = 0;

    for (; i + 16 <= size; i += 16) {
        sum += 0xA5A5A5A5u * 4u;
    }

    for (; i + 4 <= size; i += 4) {
        sum += 0xA5A5A5A5u;
    }

    for (; i < size; i++) {
        sum += MEMTEST_PATTERN;
    }

    return sum;
}

static k_u32_t speed_kbps(k_u64_t bytes, k_u64_t micros)
{
    if (micros == 0) {
        return 0;
    }

    /* bytes / microseconds = kilobytes per second */
    return (k_u32_t)((bytes * 1000u) / micros);
}

/*
 * The three operations, each repeated until the clock has moved or the
 * bound of the runs is reached. A measured interval of zero is not a speed:
 * the caller is told that the clock could not resolve the operation.
 */
static k_u32_t measure_fill(uint8_t *buffer, k_size_t size, bool *measured)
{
    k_u64_t start = k_sysinfo_uptime_us();
    k_u64_t elapsed = 0;
    k_u32_t reps = 0;

    do {
        /* A different byte each time, so the calls are not folded into one. */
        memset(buffer, (uint8_t)(MEMTEST_PATTERN + reps), size);
        reps++;
        elapsed = k_sysinfo_uptime_us() - start;
    } while (elapsed < MEMTEST_MIN_US && reps < MEMTEST_MAX_REPS);

    *measured = (elapsed > 0);

    return *measured ? speed_kbps((k_u64_t)size * reps, elapsed) : 0;
}

static k_u32_t measure_sum(uint8_t *buffer, k_size_t size, bool *measured)
{
    volatile uint8_t *flip = buffer;
    k_u64_t start = k_sysinfo_uptime_us();
    k_u64_t elapsed = 0;
    k_u32_t sum = 0;
    k_u32_t reps = 0;

    do {
        sum += checksum(buffer, size);
        /* The next read is of other bytes: the sum cannot be hoisted out of
         * the loop and timed once. */
        flip[0] ^= (uint8_t)reps;
        reps++;
        elapsed = k_sysinfo_uptime_us() - start;
    } while (elapsed < MEMTEST_MIN_US && reps < MEMTEST_MAX_REPS);

    g_sink = sum;
    *measured = (elapsed > 0);

    return *measured ? speed_kbps((k_u64_t)size * reps, elapsed) : 0;
}

static k_u32_t measure_copy(uint8_t *dst, uint8_t *src, k_size_t size, bool *measured)
{
    k_u64_t start = k_sysinfo_uptime_us();
    k_u64_t elapsed = 0;
    k_u32_t reps = 0;

    do {
        /* A source byte that changes, so the copy is not folded either. */
        src[0] = (uint8_t)(MEMTEST_PATTERN + reps);
        memcpy(dst, src, size);
        reps++;
        elapsed = k_sysinfo_uptime_us() - start;
    } while (elapsed < MEMTEST_MIN_US && reps < MEMTEST_MAX_REPS);

    *measured = (elapsed > 0);

    return *measured ? speed_kbps((k_u64_t)size * reps, elapsed) : 0;
}

/*
 * The priority of the block follows the region that is measured: the
 * kernel priority asks for the internal RAM first, the user one asks for
 * the PSRAM first.
 */
static int memtest_priority(bool psram)
{
    return psram ? KMM_PRIORITY_USER : KMM_PRIORITY_KERNEL;
}

/*
 * Allocate a test block. The internal heap of the board is small, so the
 * block is halved until it fits.
 */
static uint8_t *alloc_block(k_size_t size, bool psram, k_size_t *actual)
{
    k_size_t attempt = size;

    while (attempt >= MEMTEST_MIN_SIZE) {
        uint8_t *block = (uint8_t *)kmm_alloc(attempt, memtest_priority(psram));

        if (block != NULL) {
            *actual = attempt;
            return block;
        }

        attempt /= 2;
    }

    return NULL;
}

static void memtest_region(const char *name, bool psram, k_size_t size, struct memtest_result *out)
{
    uint8_t *buffer;
    uint8_t *copy = NULL;
    k_size_t block_size = 0;
    k_u32_t sum;

    memset(out, 0, sizeof(*out));
    out->total = psram ? k_sysinfo_psram_total() : k_sysinfo_heap_total();
    out->free_bytes = psram ? k_sysinfo_psram_free() : k_sysinfo_heap_free();
    out->used = (out->total > out->free_bytes) ? (out->total - out->free_bytes) : 0;

    if (out->total == 0) {
        /* The region does not exist on this platform. */
        return;
    }

    buffer = alloc_block(size, psram, &block_size);
    if (buffer == NULL) {
        return;
    }

    out->block_size = block_size;
    out->tested = true;

    /* The second block is only needed for the copy measurement. */
    copy = (uint8_t *)kmm_alloc(block_size, memtest_priority(psram));

    /* Write: fill the whole block, as often as the clock needs. */
    out->write_kbps = measure_fill(buffer, block_size, &out->write_measured);

    /* Read: walk through the block and sum it up. */
    out->read_kbps = measure_sum(buffer, block_size, &out->read_measured);

    /* Copy: the block into the second one. */
    if (copy != NULL) {
        out->copy_tested = true;
        out->copy_kbps = measure_copy(copy, buffer, block_size, &out->copy_measured);
    }

    /* The data has to survive the round trip: one pass of the known pattern,
     * never the varied bytes of the measurement. */
    memset(buffer, MEMTEST_PATTERN, block_size);
    sum = checksum(buffer, block_size);
    g_sink = sum;
    out->checksum_ok = (sum == expected_checksum(block_size));

    if (copy != NULL) {
        memcpy(copy, buffer, block_size);
        out->checksum_ok = out->checksum_ok && (checksum(copy, block_size) == sum);
        kmm_free(copy);
    }

    kmm_free(buffer);
}

/* One speed of the report: a number, or the truth that the clock stays still. */
static void speed_text(char *text, k_size_t size, k_u32_t kbps, bool measured, bool tested)
{
    if (!tested) {
        snprintf(text, size, "n/a");
        return;
    }

    if (!measured) {
        snprintf(text, size, "too fast to measure");
        return;
    }

    snprintf(text, size, "%u KB/s", (unsigned)kbps);
}

static void print_result(const char *name, const struct memtest_result *r)
{
    char fill[24];
    char sum[24];
    char copy[24];

    if (r->total == 0) {
        kshell_printf("%s: not available on this platform\n", name);
        return;
    }

    kshell_printf("%s: %u KB total, %u KB free, %u KB used\n", name, (unsigned)(r->total / 1024),
                  (unsigned)(r->free_bytes / 1024), (unsigned)(r->used / 1024));

    if (!r->tested) {
        kshell_printf("  not enough memory for the test block\n");
        return;
    }

    /*
     * The numbers are the speed of the three operations: the fill of the
     * block, the walk through it with the checksum and the copy of the
     * block. The checksum is limited by the additions, not by the memory.
     */
    speed_text(fill, sizeof(fill), r->write_kbps, r->write_measured, true);
    speed_text(sum, sizeof(sum), r->read_kbps, r->read_measured, true);
    speed_text(copy, sizeof(copy), r->copy_kbps, r->copy_measured, r->copy_tested);

    kshell_printf("  %u KB block: fill %s, sum %s, copy %s, data %s\n",
                  (unsigned)(r->block_size / 1024), fill, sum, copy,
                  r->checksum_ok ? "ok" : "BROKEN");
}

static int cmd_memtest(int argc, char **argv)
{
    struct memtest_result internal;
    struct memtest_result psram;
    k_size_t size = MEMTEST_DEFAULT_SIZE;

    if (argc > 1) {
        unsigned long kb = strtoul(argv[1], NULL, 10);

        if (kb == 0 || kb > (MEMTEST_MAX_SIZE / 1024)) {
            kshell_printf("usage: memtest [kilobytes], up to %u\n",
                          (unsigned)(MEMTEST_MAX_SIZE / 1024));
            return KSHELL_USAGE;
        }
        size = (k_size_t)kb * 1024;
    }

    kshell_printf("memory test, block of %u KB\n", (unsigned)(size / 1024));

    memtest_region("internal heap", false, size, &internal);
    memtest_region("psram", true, size, &psram);

    print_result("internal heap", &internal);
    print_result("psram", &psram);

    if (internal.tested && !internal.checksum_ok) {
        kshell_printf("memtest: the internal heap did not keep the data\n");
        return KSHELL_ERROR;
    }

    if (psram.tested && !psram.checksum_ok) {
        kshell_printf("memtest: the psram did not keep the data\n");
        return KSHELL_ERROR;
    }

    kshell_printf("memtest: done\n");
    return KSHELL_OK;
}

int kshell_register_memtest(void)
{
    static const struct kshell_help help = {
        .summary = "measure the memory and its speed",
        .usage = "memtest [kilobytes]",
        .detail = "Fills a block, sums it and copies it, for the internal heap and the\n"
                  "PSRAM one after the other: the sizes and the speeds are the answer.\n"
                  "The block is 32 KB unless a size is given, up to 1024 KB.\n",
    };

    return kshell_register_help("memtest", &help, cmd_memtest);
}
