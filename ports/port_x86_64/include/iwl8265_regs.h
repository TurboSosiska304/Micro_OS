/*
 * Micro OS — the portable facts of the Intel 8265 register file.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/iwl-csr.h, iwl-prph.h,
 * iwl-fh.h and pcie/trans.c (GPL-2.0, Linux v6.12);
 * code written here (MIT).
 *
 * The numbers below are the offsets, masks and decode rules of iwl-csr.h,
 * iwl-prph.h, iwl-fh.h and pcie/trans.c of Linux v6.12. They are facts about
 * the card and not the code of its driver: nothing of the algorithm is
 * carried over. This header is portable - it names registers and decodes a
 * word - while the MMIO, the PCI walk and the power sequence live in
 * src/wifi_8265.c.
 *
 * The EEPROM and OTP registers are only read as status and control words.
 * The command bit of CSR_EEPROM_REG and every write to an OTP or NVM are
 * deliberately absent: the identity of the card is in CSR_HW_REV and it is
 * read without asking the non-volatile memory for anything.
 */
/* clang-format off */
/*
Facts from drivers/net/wireless/intel/iwlwifi/... (GPL-2.0, Linux v6.12); code written here (MIT).
*/
/* clang-format on */
#ifndef MICRO_OS_IWL8265_REGS_H
#define MICRO_OS_IWL8265_REGS_H

#include "kernel/types.h"

/* --- CSR: the registers directly mapped behind BAR0 (CSR_BASE is zero) ---- */

#define IWL8265_CSR_HW_IF_CONFIG 0x000u
#define IWL8265_CSR_INT 0x008u
#define IWL8265_CSR_INT_MASK 0x00Cu
#define IWL8265_CSR_FH_INT_STATUS 0x010u
#define IWL8265_CSR_RESET 0x020u
#define IWL8265_CSR_GP_CNTRL 0x024u
#define IWL8265_CSR_HW_REV 0x028u
#define IWL8265_CSR_EEPROM_REG 0x02Cu
#define IWL8265_CSR_EEPROM_GP 0x030u
#define IWL8265_CSR_OTP_GP_REG 0x034u
#define IWL8265_CSR_MBOX_SET_REG 0x088u
#define IWL8265_CSR_GIO_CHICKEN_BITS 0x100u
#define IWL8265_CSR_DBG_HPET_MEM_REG 0x240u
#define IWL8265_CSR_DBG_LINK_PWR_MGMT_REG 0x250u

/* Bits of CSR_HW_IF_CONFIG_REG. */
#define IWL8265_HW_IF_HAP_WAKE_L1A 0x00080000u   /* bit 19: wake the PCIe link */
#define IWL8265_HW_IF_EEPROM_OWN_SEM 0x00200000u /* bit 21: status, read only */
#define IWL8265_HW_IF_NIC_READY 0x00400000u      /* bit 22: PCI owns the card */
#define IWL8265_HW_IF_PERSIST_MODE 0x40000000u   /* bit 30: keep the setup in S3 */

/* CSR_MBOX_SET_REG is a set-to-one mailbox: write the bit, never a read-back. */
#define IWL8265_MBOX_SET_OS_ALIVE 0x20u /* bit 5 */

/* Bits of CSR_GP_CNTRL. */
#define IWL8265_GP_CLOCK_READY 0x00000001u    /* bit 0 */
#define IWL8265_GP_INIT_DONE 0x00000004u      /* bit 2 */
#define IWL8265_GP_MAC_ACCESS_REQ 0x00000008u /* bit 3 */
#define IWL8265_GP_GOING_TO_SLEEP 0x00000010u /* bit 4 */

/* Bits of the two chicken registers and the PCIe link power register. */
#define IWL8265_GIO_CHICKEN_L1A_NO_L0S_RX 0x00800000u /* bit 23 */
#define IWL8265_DBG_HPET_MEM_VALUE 0xFFFF0000u
#define IWL8265_LINK_PWR_MGMT_DISABLED 0x80000000u /* bit 31: disable link PM */

/*
 * EEPROM and OTP status/control masks. They are decoded but never written
 * by this driver; in particular IWL8265_EEPROM_CMD starts a read of the
 * non-volatile memory and must stay clear.
 */
#define IWL8265_EEPROM_READ_VALID 0x00000001u
#define IWL8265_EEPROM_CMD 0x00000002u
#define IWL8265_EEPROM_GP_VALID_MASK 0x00000007u
#define IWL8265_OTP_GP_DEVICE_SELECT 0x00010000u
#define IWL8265_OTP_GP_ECC_CORR 0x00100000u
#define IWL8265_OTP_GP_ECC_UNCORR 0x00200000u

/* CSR_HW_REV: bits 15:4 are the type, the low nibble is the step of an 8000. */
#define IWL8265_HW_REV_TYPE_MASK 0x000FFF0u
#define IWL8265_HW_REV_TYPE_SHIFT 4u
#define IWL8265_HW_REV_STEP_MASK 0x0000000Fu

/*
 * The 8000 family (and the 8265 with it) keeps the step in all four low
 * bits of CSR_HW_REV. Older families used bits 3:2 as a "dash" step; that
 * decode would lose two bits of the answer of this card.
 */
static inline k_u32_t iwl8265_decode_hw_type(k_u32_t hw_rev)
{
    return (hw_rev & IWL8265_HW_REV_TYPE_MASK) >> IWL8265_HW_REV_TYPE_SHIFT;
}

static inline k_u32_t iwl8265_decode_hw_step(k_u32_t hw_rev)
{
    return hw_rev & IWL8265_HW_REV_STEP_MASK;
}

/* --- PRPH: the peripherals behind the HBUS indirect windows ---------------- */

#define IWL8265_PRPH_BASE 0x00000u
#define IWL8265_PRPH_END 0xFFFFFu
#define IWL8265_PRPH_ADDR_MASK 0x000FFFFFu
#define IWL8265_PRPH_DWORD (3u << 24) /* three bytes - 1, a four-byte access */

#define IWL8265_APMG_BASE (IWL8265_PRPH_BASE + 0x3000u)
#define IWL8265_APMG_CLK_EN_REG (IWL8265_APMG_BASE + 0x0004u)
#define IWL8265_APMG_PS_CTRL_REG (IWL8265_APMG_BASE + 0x000Cu)
#define IWL8265_APMG_PCIDEV_STT_REG (IWL8265_APMG_BASE + 0x0010u)

/* --- HBUS: the door to the PRPH and the SRAM of the device ----------------- */

#define IWL8265_HBUS_BASE 0x400u
#define IWL8265_HBUS_TARG_MEM_RADDR (IWL8265_HBUS_BASE + 0x00Cu)
#define IWL8265_HBUS_TARG_MEM_WADDR (IWL8265_HBUS_BASE + 0x010u)
#define IWL8265_HBUS_TARG_MEM_WDAT (IWL8265_HBUS_BASE + 0x018u)
#define IWL8265_HBUS_TARG_MEM_RDAT (IWL8265_HBUS_BASE + 0x01Cu)
#define IWL8265_HBUS_TARG_PRPH_WADDR (IWL8265_HBUS_BASE + 0x044u)
#define IWL8265_HBUS_TARG_PRPH_RADDR (IWL8265_HBUS_BASE + 0x048u)
#define IWL8265_HBUS_TARG_PRPH_WDAT (IWL8265_HBUS_BASE + 0x04Cu)
#define IWL8265_HBUS_TARG_PRPH_RDAT (IWL8265_HBUS_BASE + 0x050u)

/* --- FH: the DMA registers the firmware loader will use later ------------- */

#define IWL8265_FH_MEM_LOWER_BOUND 0x1000u
#define IWL8265_FH_MEM_UPPER_BOUND 0x2000u
#define IWL8265_FH_MEM_CBBC_0_15 (IWL8265_FH_MEM_LOWER_BOUND + 0x9D0u)
#define IWL8265_FH_RSCSR_CHNL0_STTS_WPTR (IWL8265_FH_MEM_LOWER_BOUND + 0xBC0u)
#define IWL8265_FH_MEM_RCSR_CHNL0_CONFIG (IWL8265_FH_MEM_LOWER_BOUND + 0xC00u)
#define IWL8265_FH_TCSR_CHNL_TX_CONFIG(q) ((IWL8265_FH_MEM_LOWER_BOUND + 0xD00u) + 0x20u * (q))
#define IWL8265_FH_TSSR_TX_STATUS (IWL8265_FH_MEM_LOWER_BOUND + 0xEB0u)

/* --- a bounded wait with injectable I/O ------------------------------------ */

/*
 * The wait is pure: a caller hands it a way to read a word and a way to read
 * the clock, and the helper only compares bits against a deadline. The card
 * supplies MMIO reads and port_x86_64_ticks(); a host test supplies two
 * arrays of numbers. No hook is exposed by the driver for this - the
 * callbacks are an argument of a header-only primitive.
 *
 * "ticks" are the milliseconds of port_x86_64_ticks(), so the budget and the
 * spent time have the same unit. `last` and `spent` are written on both
 * paths, so a caller can log the exact register, value and timeout.
 */
typedef k_u32_t (*iwl8265_mmio_read_t)(k_u32_t offset, void *opaque);
typedef void (*iwl8265_mmio_write_t)(k_u32_t offset, k_u32_t value, void *opaque);
typedef k_u64_t (*iwl8265_ticks_t)(void *opaque);

struct iwl8265_wait_io {
    iwl8265_mmio_read_t read;
    iwl8265_mmio_write_t write;
    iwl8265_ticks_t ticks;
    void *opaque;
};

static inline bool iwl8265_wait_bits(const struct iwl8265_wait_io *io, k_u32_t offset, k_u32_t mask,
                                     k_u32_t expect, k_u32_t budget_ticks, k_u32_t *last,
                                     k_u32_t *spent)
{
    k_u64_t start;
    k_u64_t now;

    if (io == NULL || io->read == NULL || io->ticks == NULL || budget_ticks == 0u) {
        return false;
    }

    start = io->ticks(io->opaque);

    for (;;) {
        k_u32_t answer = io->read(offset, io->opaque);

        if (last != NULL) {
            *last = answer;
        }

        if ((answer & mask) == expect) {
            if (spent != NULL) {
                *spent = (k_u32_t)(io->ticks(io->opaque) - start);
            }

            return true;
        }

        now = io->ticks(io->opaque);

        if (now - start >= (k_u64_t)budget_ticks) {
            if (spent != NULL) {
                *spent = (k_u32_t)(now - start);
            }

            return false;
        }
    }
}

#endif /* MICRO_OS_IWL8265_REGS_H */
