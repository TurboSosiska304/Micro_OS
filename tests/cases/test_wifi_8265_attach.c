/*
 * Micro OS — no-card contract for the Intel 8265 x86-64 probe.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/pcie/drv.c
 * (GPL-2.0, Linux v6.12); code written here (MIT).
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "port_x86_64_pci.h"
#include "port_x86_64_wifi.h"
#include "test_x86_64_ticks.h"

/* The GPU case owns the shared PCI stub; make the walk see no function. */
extern k_u16_t s_vendor;

k_u64_t microos_test_x86_64_ticks(void)
{
    return 0u;
}

bool port_x86_64_pci_find(k_u16_t vendor, k_u16_t device, struct port_x86_64_pci_device *found)
{
    (void)vendor;
    (void)device;
    (void)found;
    return false;
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
    (void)offset;
    return 0u;
}

void port_x86_64_pci_write32(const struct port_x86_64_pci_device *device, k_u8_t offset,
                             k_u32_t value)
{
    (void)device;
    (void)offset;
    (void)value;
}

void port_x86_64_pci_write16(const struct port_x86_64_pci_device *device, k_u8_t offset,
                             k_u16_t value)
{
    (void)device;
    (void)offset;
    (void)value;
}

void port_x86_64_pci_enable(const struct port_x86_64_pci_device *device, k_u16_t bits)
{
    (void)device;
    (void)bits;
}

k_u32_t port_x86_64_pci_bar_size(const struct port_x86_64_pci_device *device, k_u8_t offset)
{
    (void)device;
    (void)offset;
    return 0u;
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

bool port_x86_64_paging_kernel_present(k_u64_t vaddr)
{
    (void)vaddr;
    return false;
}

k_u64_t port_x86_64_dma_phys(const void *buffer)
{
    return port_x86_64_bus_phys(buffer);
}

bool port_x86_64_frame_hold(k_u64_t address)
{
    (void)address;
    return false;
}

void port_x86_64_frame_release(k_u64_t address)
{
    (void)address;
}

static void test_no_card(void)
{
    const struct wifi_8265_state *state;

    ktest_begin("wifi: a machine without an 8265 is skipped, not failed");
    memset(&g_port_x86_64_kernel_phys, 0, sizeof(g_port_x86_64_kernel_phys));

    s_vendor = 0xFFFFu;

    KT_CHECK_INT_EQ(wifi_8265_attach(), 0);
    state = wifi_8265_state();
    KT_CHECK(!state->present);
    KT_CHECK_INT_EQ(state->phase, WIFI_8265_PHASE_NONE);
    KT_CHECK_INT_EQ(state->error, 0);

    /* With no card there is nothing to power or wait for. */
    KT_CHECK_INT_EQ(wifi_8265_power_up(), 0);
    KT_CHECK_INT_EQ(state->phase, WIFI_8265_PHASE_NONE);
    KT_CHECK_INT_EQ(iwl8265_csr_read(IWL8265_CSR_HW_REV), 0xFFFFFFFFu);
}

void test_wifi_8265_attach(void)
{
    test_no_card();
}
