/*
 * Micro OS — the Intel GPU identity table.
 *
 * Provenance: the PCI identifiers and Kaby Lake display topology are public
 * hardware facts learned from Intel documentation and the GPL-licensed Linux
 * i915 driver. No i915 source expression was copied into this test.
 */
#include "ktest.h"
#include "tests/tests.h"

#include "gpu_intel.h"

static void test_laptop_identity(void)
{
    const struct gpu_intel_id *id = gpu_intel_id_lookup(0x8086u, 0x5917u);

    ktest_begin("gpu-intel: the ThinkPad identity is UHD 620, display 9.5");

    KT_CHECK(id != NULL);
    if (id == NULL) {
        return;
    }

    KT_CHECK_STR_EQ(id->name, "Intel UHD 620");
    KT_CHECK_INT_EQ(id->display_gen, 9);
    KT_CHECK_INT_EQ(id->display_rel, 5);
    KT_CHECK_INT_EQ(id->pipes, 3);
    KT_CHECK_INT_EQ(id->transcoders, 4);
    KT_CHECK_INT_EQ(id->ports, 5);
}

static void test_nearby_identities(void)
{
    const struct gpu_intel_id *hd = gpu_intel_id_lookup(0x8086u, 0x5916u);
    const struct gpu_intel_id *whl0 = gpu_intel_id_lookup(0x8086u, 0x3EA0u);
    const struct gpu_intel_id *whl1 = gpu_intel_id_lookup(0x8086u, 0x3EA9u);

    ktest_begin("gpu-intel: nearby Kaby Lake-R identities keep their names");

    KT_CHECK(hd != NULL);
    KT_CHECK(whl0 != NULL);
    KT_CHECK(whl1 != NULL);
    if (hd != NULL) {
        KT_CHECK_STR_EQ(hd->name, "Intel HD 620");
    }
    if (whl0 != NULL) {
        KT_CHECK_STR_EQ(whl0->name, "Intel UHD 620");
    }
    if (whl1 != NULL) {
        KT_CHECK_STR_EQ(whl1->name, "Intel UHD 620");
    }
}

static void test_rejections(void)
{
    ktest_begin("gpu-intel: another vendor or an unknown device is not this card");

    KT_CHECK(gpu_intel_id_lookup(0x1002u, 0x5917u) == NULL);
    KT_CHECK(gpu_intel_id_lookup(0x8086u, 0xFFFFu) == NULL);
    KT_CHECK(gpu_intel_id_lookup(0x1234u, 0x1111u) == NULL);
}

void test_gpu_intel(void)
{
    test_laptop_identity();
    test_nearby_identities();
    test_rejections();
}
