/*
 * Micro OS — unit tests for the AMD GPU PCI ID table.
 *
 * The lookup is pure data: no PCI, no MMIO, no port dependencies. It guards
 * the "hello GPU" probe so a known card is identified and an unknown one is
 * not claimed.
 */
#include "ktest.h"
#include "tests/tests.h"

#include "port_x86_64_gpu_ids.h"

static void test_known_ids(void)
{
    const char *name;

    ktest_begin("gpu_amd: known Ellesmere 1002:67df");

    name = port_x86_64_gpu_amd_lookup(0x1002u, 0x67DFu);
    KT_CHECK(name != NULL);
    if (name != NULL) {
        KT_CHECK_STR_EQ(name, "AMD Radeon RX 470/480/570/570X/580/580X/590");
    }

}

static void test_other_polaris_ids(void)
{
    ktest_begin("gpu_amd: other Polaris IDs resolve");

    KT_CHECK(port_x86_64_gpu_amd_lookup(0x1002u, 0x67C7u) != NULL);
    KT_CHECK(port_x86_64_gpu_amd_lookup(0x1002u, 0x67EFu) != NULL);
    KT_CHECK(port_x86_64_gpu_amd_lookup(0x1002u, 0x67FFu) != NULL);

}

static void test_unknown(void)
{
    ktest_begin("gpu_amd: unknown device is not claimed");

    KT_CHECK(port_x86_64_gpu_amd_lookup(0x1002u, 0x1234u) == NULL);
    KT_CHECK(port_x86_64_gpu_amd_lookup(0x8086u, 0x67DFu) == NULL);
    KT_CHECK(port_x86_64_gpu_amd_lookup(0x0000u, 0x0000u) == NULL);

}

static void test_table_has_live_card(void)
{
    k_size_t i;
    bool found = false;

    ktest_begin("gpu_amd: table contains live recon card 1002:67df");

    for (i = 0; i < PORT_X86_64_GPU_AMD_ID_COUNT; i++) {
        if (PORT_X86_64_GPU_AMD_IDS[i].vendor == 0x1002u &&
            PORT_X86_64_GPU_AMD_IDS[i].device == 0x67DFu) {
            found = true;
            break;
        }
    }

    KT_CHECK(found);
    KT_CHECK(PORT_X86_64_GPU_AMD_ID_COUNT >= 1u);

}

void test_gpu_amd(void)
{
    test_known_ids();
    test_other_polaris_ids();
    test_unknown();
    test_table_has_live_card();
}
