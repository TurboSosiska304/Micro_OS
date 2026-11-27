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

/*
 * The shared PCI/paging/tick stubs live in test_gpu_intel.c as weak symbols.
 * The no-card path only needs pci_find to return false and the CSR window to
 * answer all ones, which those defaults already provide.
 */
extern k_u16_t s_vendor;
extern void reset_gpu_pci_stub(void);

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
