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

#include "kernel/port.h"
#include "kernel/sched.h"

/* The size of the test block. */
#define MEMTEST_DEFAULT_SIZE (32 * 1024)
#define MEMTEST_MAX_SIZE (1024 * 1024)
#define MEMTEST_MIN_SIZE 4096

/* The pattern that is written into the block. */
#define MEMTEST_PATTERN 0xA5

struct memtest_result {
    k_size_t total;
    k_size_t free_bytes;
    k_size_t used;
    k_size_t block_size;
    k_u32_t write_kbps;
    k_u32_t read_kbps;
    k_u32_t copy_kbps;
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

static k_u32_t speed_kbps(k_size_t bytes, k_u64_t micros)
{
    if (micros == 0) {
        micros = 1;
    }

    /* bytes / microseconds = kilobytes per second */
    return (k_u32_t)((bytes * 1000u) / (k_size_t)micros);
}

/*
 * Allocate a test block. The internal heap of the board is small, so the
 * block is halved until it fits.
 */
static uint8_t *alloc_block(k_size_t size, bool psram, k_size_t *actual)
{
    k_size_t attempt = size;

    while (attempt >= MEMTEST_MIN_SIZE) {
        uint8_t *block = (uint8_t *)k_port_alloc(attempt, psram);

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
    k_u64_t t0;
    k_u64_t t1;
    k_u64_t t2;
    k_u32_t sum;

    memset(out, 0, sizeof(*out));
    out->total = psram ? k_port_psram_total() : k_port_heap_total();
    out->free_bytes = psram ? k_port_psram_free() : k_port_heap_free();
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
    copy = (uint8_t *)k_port_alloc(block_size, psram);

    /* Write: fill the whole block. */
    t0 = k_port_uptime_us();
    memset(buffer, MEMTEST_PATTERN, block_size);
    t1 = k_port_uptime_us();
    out->write_kbps = speed_kbps(block_size, t1 - t0);

    /* Read: walk through the block and sum it up. */
    t2 = k_port_uptime_us();
    sum = checksum(buffer, block_size);
    g_sink = sum;
    out->read_kbps = speed_kbps(block_size, k_port_uptime_us() - t2);

    /* Copy: the block into the second one. */
    if (copy != NULL) {
        k_u64_t c0 = k_port_uptime_us();

        memcpy(copy, buffer, block_size);
        out->copy_kbps = speed_kbps(block_size, k_port_uptime_us() - c0);
    }

    /* The data has to survive the round trip. */
    out->checksum_ok = (sum == expected_checksum(block_size));

    if (copy != NULL) {
        out->checksum_ok = out->checksum_ok && (checksum(copy, block_size) == sum);
        k_port_free(copy);
    }

    k_port_free(buffer);
}

static void print_result(const char *name, const struct memtest_result *r)
{
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
    kshell_printf("  %u KB block: fill %u KB/s, sum %u KB/s, copy %u KB/s, data %s\n",
                  (unsigned)(r->block_size / 1024), (unsigned)r->write_kbps, (unsigned)r->read_kbps,
                  (unsigned)r->copy_kbps, r->checksum_ok ? "ok" : "BROKEN");
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
    return kshell_register("memtest", "measure the memory and its speed", cmd_memtest);
}
