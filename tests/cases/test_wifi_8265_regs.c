/*
 * Micro OS — host test of the portable facts of the Intel 8265.
 *
 * The register numbers, the decoder of CSR_HW_REV, the bounded wait and the
 * phase table are checked without a card. The wait is driven by two small
 * callbacks - one returns a word, one returns the clock - so the same code
 * the driver uses is exercised here with a register that becomes ready after
 * a chosen number of reads and a clock that advances on every read.
 */
/* clang-format off */
/*
Facts from drivers/net/wireless/intel/iwlwifi/iwl-csr.h
(GPL-2.0, Linux v6.12); code written here (MIT).
*/
/* clang-format on */
#include "ktest.h"
#include "tests/tests.h"

#include "iwl8265_regs.h"
#include "port_x86_64_wifi.h"

/* The suite declares it too; keep the prototype local so this case also
 * compiles on its own, before the registration of the test is wired. */
void test_wifi_8265_regs(void);

struct fake_mmio {
    k_u32_t initial;
    k_u32_t ready_value;
    k_u32_t ready_after;
    k_u32_t tick_step;
    k_u32_t reads;
    k_u64_t ticks;
};

static k_u32_t fake_read(k_u32_t offset, void *opaque)
{
    struct fake_mmio *fake = (struct fake_mmio *)opaque;

    K_UNUSED(offset);
    fake->reads++;
    fake->ticks += fake->tick_step;

    return fake->reads >= fake->ready_after ? fake->ready_value : fake->initial;
}

static k_u64_t fake_ticks(void *opaque)
{
    return ((struct fake_mmio *)opaque)->ticks;
}

void test_wifi_8265_regs(void)
{
    struct fake_mmio fake;
    struct iwl8265_wait_io io;
    k_u32_t last;
    k_u32_t spent;
    k_u32_t step;

    /* The offsets are the CSR_BASE-relative numbers of iwl-csr.h. */
    KT_CHECK_INT_EQ(IWL8265_CSR_HW_IF_CONFIG, 0x000u);
    KT_CHECK_INT_EQ(IWL8265_CSR_INT, 0x008u);
    KT_CHECK_INT_EQ(IWL8265_CSR_INT_MASK, 0x00Cu);
    KT_CHECK_INT_EQ(IWL8265_CSR_FH_INT_STATUS, 0x010u);
    KT_CHECK_INT_EQ(IWL8265_CSR_GP_CNTRL, 0x024u);
    KT_CHECK_INT_EQ(IWL8265_CSR_HW_REV, 0x028u);
    KT_CHECK_INT_EQ(IWL8265_CSR_EEPROM_REG, 0x02Cu);
    KT_CHECK_INT_EQ(IWL8265_CSR_OTP_GP_REG, 0x034u);
    KT_CHECK_INT_EQ(IWL8265_CSR_MBOX_SET_REG, 0x088u);
    KT_CHECK_INT_EQ(IWL8265_CSR_GIO_CHICKEN_BITS, 0x100u);
    KT_CHECK_INT_EQ(IWL8265_CSR_DBG_LINK_PWR_MGMT_REG, 0x250u);

    /* The bits of the power sequence and the HBUS indirect door. */
    KT_CHECK_INT_EQ(IWL8265_HW_IF_HAP_WAKE_L1A, 0x00080000u);
    KT_CHECK_INT_EQ(IWL8265_HW_IF_NIC_READY, 0x00400000u);
    KT_CHECK_INT_EQ(IWL8265_HW_IF_PERSIST_MODE, 0x40000000u);
    KT_CHECK_INT_EQ(IWL8265_GP_MAC_ACCESS_REQ, 0x00000008u);
    KT_CHECK_INT_EQ(IWL8265_GP_GOING_TO_SLEEP, 0x00000010u);
    KT_CHECK_INT_EQ(IWL8265_MBOX_SET_OS_ALIVE, 0x20u);
    KT_CHECK_INT_EQ(IWL8265_PRPH_DWORD, 0x03000000u);
    KT_CHECK_INT_EQ(IWL8265_HBUS_TARG_PRPH_RADDR, 0x448u);
    KT_CHECK_INT_EQ(IWL8265_HBUS_TARG_PRPH_WDAT, 0x44Cu);

    /* The EEPROM command bit is never used by this driver. */
    KT_CHECK_INT_EQ(IWL8265_EEPROM_CMD, 0x00000002u);
    KT_CHECK((IWL8265_EEPROM_CMD & IWL8265_EEPROM_READ_VALID) == 0u);

    /* The type is bits 15:4; the step of an 8000 is the whole low nibble. */
    KT_CHECK_INT_EQ(iwl8265_decode_hw_type(0x000002A3u), 0x2Au);
    KT_CHECK_INT_EQ(iwl8265_decode_hw_step(0x000002A3u), 0x3u);
    KT_CHECK_INT_EQ(iwl8265_decode_hw_type(0x000FFF0u), 0xFFFu);
    KT_CHECK_INT_EQ(iwl8265_decode_hw_step(0x000FFF0u), 0x0u);
    KT_CHECK_INT_EQ(iwl8265_decode_hw_step(0x000000Cu), 0xCu);
    for (step = 0u; step < 16u; step++) {
        KT_CHECK_INT_EQ(iwl8265_decode_hw_step(0x000002A0u | step), step);
    }

    /* A wait that is already satisfied reports the first value and no time. */
    fake = (struct fake_mmio){0u, IWL8265_GP_CLOCK_READY, 1u, 1u, 0u, 0u};
    io = (struct iwl8265_wait_io){fake_read, NULL, fake_ticks, &fake};
    last = 0u;
    spent = 99u;
    KT_CHECK(iwl8265_wait_bits(&io, IWL8265_CSR_GP_CNTRL, IWL8265_GP_CLOCK_READY,
                               IWL8265_GP_CLOCK_READY, 15u, &last, &spent));
    KT_CHECK_INT_EQ(last, IWL8265_GP_CLOCK_READY);
    KT_CHECK_INT_EQ(spent, 1u);
    KT_CHECK_INT_EQ(fake.reads, 1u);

    /* A card that answers after two reads is inside the budget. */
    fake = (struct fake_mmio){0u, IWL8265_HW_IF_NIC_READY, 3u, 2u, 0u, 0u};
    io = (struct iwl8265_wait_io){fake_read, NULL, fake_ticks, &fake};
    last = 0u;
    spent = 0u;
    KT_CHECK(iwl8265_wait_bits(&io, IWL8265_CSR_HW_IF_CONFIG, IWL8265_HW_IF_NIC_READY,
                               IWL8265_HW_IF_NIC_READY, 50u, &last, &spent));
    KT_CHECK_INT_EQ(last, IWL8265_HW_IF_NIC_READY);
    KT_CHECK_INT_EQ(spent, 6u);
    KT_CHECK_INT_EQ(fake.reads, 3u);

    /* A card that never answers stops at the deadline and reports its last
     * word and the milliseconds it spent, so the failure can be logged. */
    fake = (struct fake_mmio){0u, 0u, 100u, 5u, 0u, 0u};
    io = (struct iwl8265_wait_io){fake_read, NULL, fake_ticks, &fake};
    last = 0xFFFFFFFFu;
    spent = 0u;
    KT_CHECK(!iwl8265_wait_bits(&io, IWL8265_CSR_GP_CNTRL,
                                IWL8265_GP_CLOCK_READY | IWL8265_GP_GOING_TO_SLEEP,
                                IWL8265_GP_CLOCK_READY, 15u, &last, &spent));
    KT_CHECK_INT_EQ(last, 0u);
    KT_CHECK_INT_EQ(spent, 15u);
    KT_CHECK_INT_EQ(fake.reads, 3u);

    /* The mask includes GOING_TO_SLEEP: a clock that is ready while the
     * card is going to sleep is not a good time to touch a register. */
    fake = (struct fake_mmio){IWL8265_GP_CLOCK_READY | IWL8265_GP_GOING_TO_SLEEP,
                              IWL8265_GP_GOING_TO_SLEEP,
                              1u,
                              1u,
                              0u,
                              0u};
    io = (struct iwl8265_wait_io){fake_read, NULL, fake_ticks, &fake};
    KT_CHECK(!iwl8265_wait_bits(&io, IWL8265_CSR_GP_CNTRL,
                                IWL8265_GP_CLOCK_READY | IWL8265_GP_GOING_TO_SLEEP,
                                IWL8265_GP_CLOCK_READY, 15u, NULL, NULL));

    /* The phase names are the ones the shell prints, and a failed step
     * leaves the card in FAILED instead of moving it to the next phase. */
    KT_CHECK_STR_EQ(wifi_8265_phase_name(WIFI_8265_PHASE_NONE), "none");
    KT_CHECK_STR_EQ(wifi_8265_phase_name(WIFI_8265_PHASE_ATTACHED), "attached");
    KT_CHECK_STR_EQ(wifi_8265_phase_name(WIFI_8265_PHASE_POWERED), "powered");
    KT_CHECK_STR_EQ(wifi_8265_phase_name(WIFI_8265_PHASE_FIRMWARE_READY), "firmware-ready");
    KT_CHECK_STR_EQ(wifi_8265_phase_name(WIFI_8265_PHASE_ALIVE), "alive");
    KT_CHECK_STR_EQ(wifi_8265_phase_name(WIFI_8265_PHASE_FAILED), "failed");

    KT_CHECK_INT_EQ(wifi_8265_phase_after(WIFI_8265_PHASE_NONE, true), WIFI_8265_PHASE_ATTACHED);
    KT_CHECK_INT_EQ(wifi_8265_phase_after(WIFI_8265_PHASE_ATTACHED, true), WIFI_8265_PHASE_POWERED);
    KT_CHECK_INT_EQ(wifi_8265_phase_after(WIFI_8265_PHASE_POWERED, true),
                    WIFI_8265_PHASE_FIRMWARE_READY);
    KT_CHECK_INT_EQ(wifi_8265_phase_after(WIFI_8265_PHASE_FIRMWARE_READY, true),
                    WIFI_8265_PHASE_ALIVE);
    KT_CHECK_INT_EQ(wifi_8265_phase_after(WIFI_8265_PHASE_NONE, false), WIFI_8265_PHASE_FAILED);
    KT_CHECK_INT_EQ(wifi_8265_phase_after(WIFI_8265_PHASE_ATTACHED, false), WIFI_8265_PHASE_FAILED);
    KT_CHECK_INT_EQ(wifi_8265_phase_after(WIFI_8265_PHASE_FAILED, true), WIFI_8265_PHASE_FAILED);
    KT_CHECK_INT_EQ(wifi_8265_phase_after(WIFI_8265_PHASE_ALIVE, true), WIFI_8265_PHASE_ALIVE);
}
