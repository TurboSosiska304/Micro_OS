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
#include "kernel/log.h"
#include "port_x86_64_pci.h"

/* The empty bus used by the no-card case; the real probe is linked here. */
static k_u32_t s_find_calls;

bool port_x86_64_pci_find(k_u16_t vendor, k_u16_t device, struct port_x86_64_pci_device *found)
{
    K_UNUSED(vendor);
    K_UNUSED(device);
    K_UNUSED(found);
    s_find_calls++;
    return false;
}

k_u32_t port_x86_64_pci_read32(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    K_UNUSED(device);
    K_UNUSED(offset);
    return 0u;
}

k_u16_t port_x86_64_pci_read16(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    K_UNUSED(device);
    K_UNUSED(offset);
    return 0u;
}

void port_x86_64_pci_enable(const struct port_x86_64_pci_device *device, k_u16_t bits)
{
    K_UNUSED(device);
    K_UNUSED(bits);
}

k_u32_t port_x86_64_paging_map_mmio(k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    K_UNUSED(vaddr);
    K_UNUSED(paddr);
    K_UNUSED(len);
    return 0u;
}

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

static void test_absent_card(void)
{
    struct gpu_intel_info info;
    const char *captured;
    kmod_state_t state;

    ktest_begin("gpu-intel: an empty PCI bus is an honest KMOD_SKIP");

    s_find_calls = 0u;
    klog_capture_start();
    state = gpu_intel_probe(&info);
    klog_capture_stop();
    captured = klog_captured_text();

    KT_CHECK_INT_EQ(state, KMOD_SKIP);
    KT_CHECK(info.name == NULL);
    KT_CHECK_INT_EQ(s_find_calls, 5);
    KT_CHECK(captured != NULL);
    if (captured != NULL) {
        KT_CHECK(strstr(captured, "gpu: no supported Intel display found (KMOD_SKIP)") != NULL);
    }
}

void test_gpu_intel(void)
{
    test_laptop_identity();
    test_nearby_identities();
    test_rejections();
    test_absent_card();
}
