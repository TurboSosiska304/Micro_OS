/*
 * Micro OS — host test of the Intel 8265 power sequence.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/iwl-csr.h and pcie/trans.c
 * (GPL-2.0, Linux v6.12); code written here (MIT).
 */
#include "ktest.h"
#include "tests/tests.h"

#include "kernel/errno.h"
#include "port_x86_64_wifi.h"
#include "test_x86_64_ticks.h"

void test_wifi_8265_regs(void);

#define FAKE_REGS 0x400u
#define FAKE_WRITES 64u

struct fake_power {
    k_u32_t regs[FAKE_REGS];
    k_u32_t writes[FAKE_WRITES];
    k_u32_t write_count;
    k_u64_t ticks;
    bool ready;
};

static k_u32_t power_read(k_u32_t offset, void *opaque)
{
    struct fake_power *fake = (struct fake_power *)opaque;

    if (offset == IWL8265_CSR_HW_IF_CONFIG && fake->ready) {
        fake->regs[offset / 4u] |= IWL8265_HW_IF_NIC_READY;
    }
    if (offset == IWL8265_CSR_GP_CNTRL && fake->ready) {
        fake->regs[offset / 4u] |= IWL8265_GP_CLOCK_READY;
    }
    fake->ticks++;
    return fake->regs[offset / 4u];
}

static void power_write(k_u32_t offset, k_u32_t value, void *opaque)
{
    struct fake_power *fake = (struct fake_power *)opaque;

    if (fake->write_count < FAKE_WRITES) {
        fake->writes[fake->write_count] = offset;
    }
    fake->write_count++;
    if (offset != IWL8265_CSR_MBOX_SET_REG) {
        fake->regs[offset / 4u] = value;
    }
}

static k_u64_t power_ticks(void *opaque)
{
    return ((struct fake_power *)opaque)->ticks;
}

static void test_power_order(void)
{
    struct fake_power fake;
    struct iwl8265_wait_io io;
    k_u32_t hw_config = 0u;
    k_u32_t gp = 0u;
    k_size_t i;
    bool saw_ready = false;
    bool saw_mailbox = false;
    bool saw_access = false;
    bool saw_init = false;
    bool saw_persist = false;

    ktest_begin("wifi: the bounded power sequence writes ownership before clocks");
    memset(&fake, 0, sizeof(fake));
    fake.ready = true;
    io = (struct iwl8265_wait_io){power_read, power_write, power_ticks, &fake};

    KT_CHECK_INT_EQ(wifi_8265_power_sequence(&io, &hw_config, &gp), KERR_NONE);
    KT_CHECK(hw_config & IWL8265_HW_IF_PERSIST_MODE);
    KT_CHECK(gp & IWL8265_GP_INIT_DONE);
    KT_CHECK(fake.write_count >= 9u);

    for (i = 0u; i < fake.write_count && i < FAKE_WRITES; i++) {
        k_u32_t offset = fake.writes[i];
        if (offset == IWL8265_CSR_HW_IF_CONFIG &&
            (fake.regs[offset / 4u] & IWL8265_HW_IF_NIC_READY) != 0u) {
            saw_ready = true;
        }
        if (offset == IWL8265_CSR_MBOX_SET_REG) {
            saw_mailbox = true;
        }
        if (offset == IWL8265_CSR_GP_CNTRL &&
            (fake.regs[offset / 4u] & IWL8265_GP_MAC_ACCESS_REQ) != 0u) {
            saw_access = true;
        }
        if (offset == IWL8265_CSR_GP_CNTRL &&
            (fake.regs[offset / 4u] & IWL8265_GP_INIT_DONE) != 0u) {
            saw_init = true;
        }
        if (offset == IWL8265_CSR_HW_IF_CONFIG &&
            (fake.regs[offset / 4u] & IWL8265_HW_IF_PERSIST_MODE) != 0u) {
            saw_persist = true;
        }
    }
    KT_CHECK(saw_ready);
    KT_CHECK(saw_mailbox);
    KT_CHECK(saw_access);
    KT_CHECK(saw_init);
    KT_CHECK(saw_persist);
}

static void test_power_timeout(void)
{
    struct fake_power fake;
    struct iwl8265_wait_io io;
    k_u32_t hw_config = 0u;
    k_u32_t gp = 0u;

    ktest_begin("wifi: a card that never owns the semaphore stops at the bound");
    memset(&fake, 0, sizeof(fake));
    io = (struct iwl8265_wait_io){power_read, power_write, power_ticks, &fake};

    KT_CHECK_INT_EQ(wifi_8265_power_sequence(&io, &hw_config, &gp), KERR_IO);
    KT_CHECK_INT_EQ(fake.ticks, 21u);
    KT_CHECK_INT_EQ(fake.write_count, 7u);
}

void test_wifi_8265_power(void)
{
    test_power_order();
    test_power_timeout();
}
