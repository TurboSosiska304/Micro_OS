/*
 * Micro OS — the Intel GPU identity table and probe contract.
 *
 * Facts from drivers/gpu/drm/i915/i915_pciids.h,
 * drivers/gpu/drm/i915/intel_step.c and
 * drivers/gpu/drm/i915/display/intel_display_device.c (GPL-2.0, Linux v6.12);
 * code written here for Micro OS (MIT).
 */
#include "ktest.h"
#include "tests/tests.h"

#include "gpu_intel.h"
#include "port_x86_64.h"
#include "port_x86_64_pci.h"

#include <string.h>

static k_u16_t s_vendor;
static k_u16_t s_device;
static k_u8_t s_revision;

/* The host case links the Intel source but not the x86-64 paging/timer port. */
k_u64_t g_port_x86_64_kernel_phys;
static k_u64_t s_ticks;

k_u64_t port_x86_64_ticks(void)
{
    s_ticks++;
    return s_ticks;
}

k_u32_t port_x86_64_pci_read32(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    (void)device;
    (void)offset;
    return 0u;
}

k_u16_t port_x86_64_pci_read16(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    (void)device;
    if (offset == PCI_VENDOR) {
        return s_vendor;
    }
    if (offset == PCI_DEVICE) {
        return s_device;
    }
    if (offset == PCI_REVISION) {
        return (k_u16_t)s_revision;
    }
    if (offset == PCI_CLASS) {
        return 0x0300u;
    }
    return 0u;
}

void port_x86_64_pci_enable(const struct port_x86_64_pci_device *device, k_u16_t bits)
{
    (void)device;
    (void)bits;
}

k_u32_t port_x86_64_paging_map_mmio(k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    (void)vaddr;
    (void)paddr;
    (void)len;
    return 0u;
}

k_u32_t port_x86_64_paging_map_framebuffer(k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    (void)vaddr;
    (void)paddr;
    (void)len;
    return 0u;
}

k_u64_t port_x86_64_dma_phys(const void *buffer)
{
    return port_x86_64_bus_phys(buffer);
}

bool port_x86_64_frame_hold(k_u64_t address)
{
    (void)address;
    return true;
}

void port_x86_64_frame_release(k_u64_t address)
{
    (void)address;
}

void port_x86_64_fb_set_format(k_u8_t format, k_u8_t bpp);
void port_x86_64_fb_set_format(k_u8_t format, k_u8_t bpp)
{
    (void)format;
    (void)bpp;
}

static void test_laptop_identity(void)
{
    const struct gpu_intel_id *id = gpu_intel_id_lookup(0x8086u, 0x5917u);

    ktest_begin("gpu-intel: the ThinkPad identity is UHD 620, display 9.00");

    KT_CHECK(id != NULL);
    if (id == NULL) {
        return;
    }

    KT_CHECK_STR_EQ(id->name, "Intel UHD 620");
    KT_CHECK_INT_EQ(id->display_gen, 9);
    KT_CHECK_INT_EQ(id->display_rel, 0);
    KT_CHECK_INT_EQ(id->pipes, 3);
    KT_CHECK_INT_EQ(id->transcoders, 4);
    KT_CHECK_INT_EQ(id->ports, 5);
    KT_CHECK_INT_EQ(id->mmio_size, 16u * 1024u * 1024u);
}

static void test_nearby_identities(void)
{
    const struct gpu_intel_id *hd = gpu_intel_id_lookup(0x8086u, 0x5916u);
    const struct gpu_intel_id *whl0 = gpu_intel_id_lookup(0x8086u, 0x3EA0u);
    const struct gpu_intel_id *whl1 = gpu_intel_id_lookup(0x8086u, 0x3EA9u);
    const struct gpu_intel_id *ult = gpu_intel_id_lookup(0x8086u, 0x87C0u);

    ktest_begin("gpu-intel: nearby Kaby Lake-R identities keep their names");

    KT_CHECK(hd != NULL);
    KT_CHECK(whl0 != NULL);
    KT_CHECK(whl1 != NULL);
    KT_CHECK(ult != NULL);
    if (hd != NULL) {
        KT_CHECK_STR_EQ(hd->name, "Intel HD 620");
    }
    if (whl0 != NULL) {
        KT_CHECK_STR_EQ(whl0->name, "Intel UHD 620");
    }
    if (whl1 != NULL) {
        KT_CHECK_STR_EQ(whl1->name, "Intel UHD 620");
    }
    if (ult != NULL) {
        KT_CHECK_STR_EQ(ult->name, "Intel UHD 617");
    }
}

static void test_rejections(void)
{
    ktest_begin("gpu-intel: another vendor or an unknown device is not this card");

    KT_CHECK(gpu_intel_id_lookup(0x1002u, 0x5917u) == NULL);
    KT_CHECK(gpu_intel_id_lookup(0x8086u, 0xFFFFu) == NULL);
    KT_CHECK(gpu_intel_id_lookup(0x1234u, 0x1111u) == NULL);
}

static void test_probe_none_for_other_vendor(void)
{
    struct port_x86_64_pci_device pci = {0u, 2u, 0u};
    struct gpu_info info;
    enum gpu_probe_result res;

    ktest_begin("gpu-intel: non-Intel vendor is not this probe's responsibility");

    s_vendor = 0x1002u;
    s_device = 0x67DFu;
    s_revision = 0xEFu;
    memset(&info, 0, sizeof(info));
    res = gpu_intel_probe(&pci, &info);
    KT_CHECK_INT_EQ((int)res, (int)GPU_PROBE_NONE);
}

static void test_probe_unknown_intel(void)
{
    struct port_x86_64_pci_device pci = {0u, 2u, 0u};
    struct gpu_info info;
    enum gpu_probe_result res;

    ktest_begin("gpu-intel: unknown Intel DID is GPU_PROBE_UNKNOWN_DEV");

    s_vendor = 0x8086u;
    s_device = 0xFFFFu;
    s_revision = 0x07u;
    memset(&info, 0, sizeof(info));
    res = gpu_intel_probe(&pci, &info);

    KT_CHECK_INT_EQ((int)res, (int)GPU_PROBE_UNKNOWN_DEV);
    KT_CHECK(info.present);
    KT_CHECK(!info.known);
    KT_CHECK_INT_EQ(info.vendor, 0x8086u);
    KT_CHECK_INT_EQ(info.device, 0xFFFFu);
    KT_CHECK_INT_EQ(info.revision, 0x07u);
    KT_CHECK_INT_EQ(info.bus, 0u);
    KT_CHECK_INT_EQ(info.slot, 2u);
    KT_CHECK_INT_EQ(info.function, 0u);
}

static void test_probe_known_intel_degraded(void)
{
    struct port_x86_64_pci_device pci = {0u, 2u, 0u};
    struct gpu_info info;
    enum gpu_probe_result res;

    ktest_begin("gpu-intel: a known laptop fills the shared result before degrading");

    s_vendor = 0x8086u;
    s_device = 0x5917u;
    s_revision = 0x07u;
    memset(&info, 0, sizeof(info));
    res = gpu_intel_probe(&pci, &info);

    KT_CHECK_INT_EQ((int)res, (int)GPU_PROBE_DEGRADED);
    KT_CHECK(info.present);
    KT_CHECK(info.known);
    KT_CHECK_STR_EQ(info.name, "Intel UHD 620");
    KT_CHECK_INT_EQ(info.mmio_size, 16u * 1024u * 1024u);
    KT_CHECK_INT_EQ(info.display_gen, 9);
    KT_CHECK_INT_EQ(info.display_rel, 0);
    KT_CHECK_STR_EQ(info.display_step, "C0");
    KT_CHECK_INT_EQ(info.probe_result, GPU_PROBE_DEGRADED);
}

void test_gpu_intel(void)
{
    test_laptop_identity();
    test_nearby_identities();
    test_rejections();
    test_probe_none_for_other_vendor();
    test_probe_unknown_intel();
    test_probe_known_intel_degraded();
}
