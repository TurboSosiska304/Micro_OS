/*
 * Micro OS — the contract of the DMA allocator of the port.
 *
 * A device reads and writes the memory by its own address, so the block has
 * to be aligned as the device asks and it has to stay there until it is
 * freed. The host and the x86-64 port both provide the contract; this test
 * only checks the portable part of it.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/port.h"

static void test_alignment(void)
{
    static const k_size_t aligns[] = {4u, 16u, 64u, 256u, 4096u};
    k_size_t i;

    ktest_begin("port: dma alloc returns the alignment the device asked for");

    for (i = 0u; i < sizeof(aligns) / sizeof(aligns[0]); i++) {
        void *ptr = k_port_dma_alloc(128u, aligns[i]);
        uintptr_t value;

        KT_CHECK(ptr != NULL);
        if (ptr == NULL) {
            continue;
        }
        value = (uintptr_t)ptr;
        KT_CHECK_INT_EQ((int)(value & (aligns[i] - 1u)), 0);
        memset(ptr, 0x5Au, 128u);
        k_port_dma_free(ptr);
    }
}

static void test_size_and_reuse(void)
{
    void *first;
    void *second;
    k_u8_t *bytes;
    k_size_t i;

    ktest_begin("port: dma free returns the block and a repeat alloc does not fail");

    first = k_port_dma_alloc(4096u, 4096u);
    KT_CHECK(first != NULL);
    if (first == NULL) {
        return;
    }

    bytes = (k_u8_t *)first;
    for (i = 0u; i < 4096u; i++) {
        bytes[i] = (k_u8_t)(i & 0xFFu);
    }
    for (i = 0u; i < 4096u; i++) {
        KT_CHECK_INT_EQ(bytes[i], (k_u8_t)(i & 0xFFu));
    }

    k_port_dma_free(first);

    second = k_port_dma_alloc(4096u, 4096u);
    KT_CHECK(second != NULL);
    if (second != NULL) {
        memset(second, 0xA5u, 4096u);
        k_port_dma_free(second);
    }
}

static void test_invalid(void)
{
    ktest_begin("port: dma alloc refuses an invalid request");

    KT_CHECK(k_port_dma_alloc(0u, 16u) == NULL);
    KT_CHECK(k_port_dma_alloc(16u, 0u) == NULL);
    KT_CHECK(k_port_dma_alloc(16u, 3u) == NULL);
    k_port_dma_free(NULL);
}

void test_port_dma(void)
{
    test_alignment();
    test_size_and_reuse();
    test_invalid();
}
