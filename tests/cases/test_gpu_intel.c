/*
 * Micro OS — the Intel GPU identity table and probe states.
 *
 * Facts from drivers/gpu/drm/i915/i915_pciids.h,
 * drivers/gpu/drm/i915/intel_step.c and
 * drivers/gpu/drm/i915/display/intel_display_device.c (GPL-2.0, Linux v6.12);
 * code written here for Micro OS (MIT).
 */
#include "ktest.h"
#include "tests/tests.h"

#include "gpu_intel.h"
#include "kernel/log.h"
#include "port_x86_64_pci.h"

enum test_bus {
    TEST_BUS_EMPTY,
    TEST_BUS_UNKNOWN_INTEL,
};

/* The fake bus used by the no-card and unknown-ID cases; the real probe is linked. */
static enum test_bus s_bus;
static k_u32_t s_find_calls;
static k_u32_t s_class_calls;
static k_u32_t s_map_calls;
static k_u32_t s_fb_map_calls;

/* The host case links the Intel source but not the x86-64 paging port. */
k_u64_t g_port_x86_64_kernel_phys;
static k_u64_t s_ticks;

k_u64_t port_x86_64_ticks(void)
{
    s_ticks++;
    return s_ticks;
}

bool port_x86_64_pci_find(k_u16_t vendor, k_u16_t device, struct port_x86_64_pci_device *found)
{
    K_UNUSED(vendor);
    K_UNUSED(device);
    K_UNUSED(found);
    s_find_calls++;
    return false;
}

bool port_x86_64_pci_find_class(k_u8_t base, k_u8_t sub, k_u8_t prog_if,
                                struct port_x86_64_pci_device *found)
{
    K_UNUSED(base);
    K_UNUSED(sub);
    K_UNUSED(prog_if);
    s_class_calls++;
    if (s_bus != TEST_BUS_UNKNOWN_INTEL) {
        return false;
    }
    *found = (struct port_x86_64_pci_device){0u, 2u, 0u};
    return true;
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
    if (s_bus != TEST_BUS_UNKNOWN_INTEL) {
        return 0u;
    }
    if (offset == PCI_VENDOR) {
        return 0x8086u;
    }
    if (offset == PCI_DEVICE) {
        return 0xFFFFu;
    }
    if (offset == PCI_CLASS) {
        return 0x0300u;
    }
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
    s_map_calls++;
    return 0u;
}

k_u32_t port_x86_64_paging_map_framebuffer(k_u64_t vaddr, k_u64_t paddr, k_size_t len)
{
    K_UNUSED(vaddr);
    K_UNUSED(paddr);
    K_UNUSED(len);
    s_fb_map_calls++;
    return 0u;
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

static void test_absent_card(void)
{
    struct gpu_info info;
    const char *captured;
    int rc;

    ktest_begin("gpu-intel: an empty PCI bus is an honest KMOD_SKIP");

    s_bus = TEST_BUS_EMPTY;
    s_find_calls = 0u;
    s_class_calls = 0u;
    s_map_calls = 0u;
    klog_capture_start();
    rc = gpu_intel_probe(&info);
    klog_capture_stop();
    captured = klog_captured_text();

    KT_CHECK_INT_EQ(rc, 1);
    KT_CHECK_INT_EQ(info.state, KMOD_SKIP);
    KT_CHECK(info.vendor == 0u);
    KT_CHECK(info.vram_bytes == GPU_INFO_UNKNOWN_SIZE);
    KT_CHECK_INT_EQ(s_find_calls, 5);
    KT_CHECK_INT_EQ(s_class_calls, 1);
    KT_CHECK_INT_EQ(s_map_calls, 0u);
    KT_CHECK(captured != NULL);
    if (captured != NULL) {
        KT_CHECK(strstr(captured, "gpu: no Intel display found (KMOD_SKIP)") != NULL);
    }
}

static void test_unknown_intel(void)
{
    struct gpu_info info;
    const char *captured;
    int rc;

    ktest_begin("gpu-intel: an unknown Intel DID is KMOD_UNKNOWN, not KMOD_SKIP");

    s_bus = TEST_BUS_UNKNOWN_INTEL;
    s_find_calls = 0u;
    s_class_calls = 0u;
    s_map_calls = 0u;
    klog_capture_start();
    rc = gpu_intel_probe(&info);
    klog_capture_stop();
    captured = klog_captured_text();

    KT_CHECK_INT_EQ(rc, 1);
    KT_CHECK_INT_EQ(info.state, KMOD_UNKNOWN);
    KT_CHECK_INT_EQ(info.vendor, 0x8086u);
    KT_CHECK_INT_EQ(info.device, 0xFFFFu);
    KT_CHECK_INT_EQ(info.bus, 0u);
    KT_CHECK_INT_EQ(info.slot, 2u);
    KT_CHECK_INT_EQ(info.func, 0u);
    KT_CHECK(info.mmio_size == 0u);
    KT_CHECK(info.vram_bytes == GPU_INFO_UNKNOWN_SIZE);
    KT_CHECK_INT_EQ(s_class_calls, 1);
    KT_CHECK_INT_EQ(s_map_calls, 0u);
    KT_CHECK(captured != NULL);
    if (captured != NULL) {
        KT_CHECK(strstr(captured, "is not in the probe table (KMOD_UNKNOWN)") != NULL);
    }
}

void test_gpu_intel(void)
{
    test_laptop_identity();
    test_nearby_identities();
    test_rejections();
    test_absent_card();
    test_unknown_intel();
}
