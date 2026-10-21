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
#define MEMTEST_DEFAULT_SIZE (64 * 1024)
#define MEMTEST_MAX_SIZE (512 * 1024)

/* The pattern that is written into the block. */
#define MEMTEST_PATTERN 0xA5

/* The speed is printed in KB/s, the resolution is enough for the display. */
#define MEMTEST_UNIT 1024

struct memtest_result {
    k_size_t total;
    k_size_t free_bytes;
    k_size_t used;
    k_u32_t write_kbps;
    k_u32_t read_kbps;
    k_u32_t copy_kbps;
    bool checksum_ok;
};

/* The sum of the block, so that the compiler cannot skip the read. */
static volatile k_u32_t g_sink;

static k_u32_t checksum(const uint8_t *data, k_size_t size)
{
    k_u32_t sum = 0;
    k_size_t i;

    for (i = 0; i < size; i++) {
        sum += data[i];
    }

    return sum;
}

static k_u32_t speed_kbps(k_size_t bytes, k_u64_t micros)
{
    if (micros == 0) {
        micros = 1;
    }

    /* bytes / micros = bytes per microsecond = KB/s */
    return (k_u32_t)((bytes * 1000u) / (k_size_t)micros);
}

static void memtest_region(const char *name, bool psram, k_size_t size, struct memtest_result *out)
{
    uint8_t *buffer;
    uint8_t *copy;
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

    buffer = (uint8_t *)k_port_alloc(size, psram);
    copy = (uint8_t *)k_port_alloc(size, psram);
    if (buffer == NULL || copy == NULL) {
        k_port_free(buffer);
        k_port_free(copy);
        return;
    }

    /* Write: fill the whole block. */
    t0 = k_port_uptime_us();
    memset(buffer, MEMTEST_PATTERN, size);
    t1 = k_port_uptime_us();

    /* Read: walk through the block and sum it up. */
    t2 = k_port_uptime_us();
    sum = checksum(buffer, size);
    g_sink = sum;
    {
        k_u64_t t3 = k_port_uptime_us();

        out->read_kbps = speed_kbps(size, t3 - t2);
    }

    out->write_kbps = speed_kbps(size, t1 - t0);

    /* Copy: the block into the second one. */
    {
        k_u64_t c0 = k_port_uptime_us();

        memcpy(copy, buffer, size);
        {
            k_u64_t c1 = k_port_uptime_us();

            out->copy_kbps = speed_kbps(size, c1 - c0);
        }
    }

    /* The data has to survive the round trip. */
    out->checksum_ok = (sum == (k_u32_t)(MEMTEST_PATTERN * size)) && (checksum(copy, size) == sum);

    k_port_free(copy);
    k_port_free(buffer);
}

static void print_result(const char *name, const struct memtest_result *r, k_size_t size)
{
    if (r->total == 0) {
        kshell_printf("%s: not available on this platform\n", name);
        return;
    }

    kshell_printf("%s: %u KB total, %u KB free, %u KB used\n", name, (unsigned)(r->total / 1024),
                  (unsigned)(r->free_bytes / 1024), (unsigned)(r->used / 1024));

    if (r->write_kbps == 0 && r->read_kbps == 0) {
        kshell_printf("  no memory for the test block of %u KB\n", (unsigned)(size / 1024));
        return;
    }

    kshell_printf("  %u KB block: write %u KB/s, read %u KB/s, copy %u KB/s, data %s\n",
                  (unsigned)(size / 1024), (unsigned)r->write_kbps, (unsigned)r->read_kbps,
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

    print_result("internal heap", &internal, size);
    print_result("psram", &psram, size);

    if (internal.total > 0 && !internal.checksum_ok) {
        kshell_printf("memtest: the internal heap did not keep the data\n");
        return KSHELL_ERROR;
    }

    if (psram.total > 0 && !psram.checksum_ok) {
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
