/*
 * Micro OS — the x86-64 port: attach and bounded power probe of the Intel 8265.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/iwl-csr.h, iwl-prph.h and
 * pcie/trans.c (GPL-2.0, Linux v6.12); code written here (MIT).
 *
 * The card is a PCI function (8086:24fd) with a register window of at least
 * 8 KiB in BAR0. This file owns the machine half of the probe: the walk of the
 * configuration space, the uncached map of BAR0, the MMIO and the power
 * sequence. The register numbers and the pure wait are in iwl8265_regs.h.
 *
 * The probe is deliberately read-only where it can be. The EEPROM and OTP
 * registers are read as status and control words and nothing is written to
 * start an EEPROM read or an OTP/NVM write: CSR_HW_REV answers the identity
 * without the non-volatile memory. No reset is issued on this path either,
 * so a card that is already owned by another component of the machine is
 * left as it was.
 *
 * The 8265 is an 8000-family chip with apmg_not_supported set in its Linux
 * configuration, so the APMG peripheral registers are not touched. The
 * clocks are brought up through the CSR/GP_CNTRL sequence of iwl_finish_nic_init
 * and iwl_pcie_set_hw_ready instead, with the timeouts of the driver.
 *
 * QEMU has no model of this card: the walk finds nothing and the two calls
 * return quietly. A machine with the card passed through is the only case
 * where the MMIO below is real; the writes are the documented ones of the
 * driver of Linux, and the waits are bounded by the tick counter of the port.
 */
/* clang-format off */
/*
Facts from drivers/net/wireless/intel/iwlwifi/... (GPL-2.0, Linux v6.12); code written here (MIT).
*/
/* clang-format on */
#include "port_x86_64.h"

#include "kernel/errno.h"
#include "kernel/log.h"
#include "port_x86_64_pci.h"
#include "port_x86_64_wifi.h"

#ifdef MICRO_OS_HOST_TEST
#include "test_x86_64_ticks.h"
#else
#define wifi_test_ticks port_x86_64_ticks
#endif

#define WIFI_8265_VENDOR_INTEL 0x8086u
#define WIFI_8265_DEVICE 0x24FDu

/* The configuration-space registers of the power capability and of the
 * command word; the port's PCI header only names the ones every driver uses. */
#define WIFI_8265_PCI_STATUS 0x06u
#define WIFI_8265_PCI_STATUS_CAP_LIST 0x0010u
#define WIFI_8265_PCI_CAP_POINTER 0x34u
#define WIFI_8265_PCI_CAP_ID_POWER 0x01u
#define WIFI_8265_PCI_PMCSR_POWER_MASK 0x0003u
#define WIFI_8265_PCI_COMMAND_INT_DISABLE 0x0400u
#define WIFI_8265_PCI_CAP_GUARD 48u

#define WIFI_8265_MIN_BAR_SIZE 0x2000u
#define WIFI_8265_LARGE_PAGE_MASK 0x1FFFFFull
#define WIFI_8265_D0_SETTLE_MS 10u
#define WIFI_8265_HW_READY_MS 50u
#define WIFI_8265_MAC_ACCESS_MS 15u
#define WIFI_8265_CLOCK_READY_MS 25u

static struct wifi_8265_state g_state;
static volatile k_u32_t *g_regs;
static k_u32_t g_window_size;

/* --- the state of the card ------------------------------------------------- */

const struct wifi_8265_state *wifi_8265_state(void)
{
    return &g_state;
}

/* A short reason for the shell, without a formatter in the kernel. */
static void wifi_8265_reason(const char *text)
{
    k_size_t i;

    if (text == NULL) {
        g_state.reason[0] = '\0';
        return;
    }
    for (i = 0u; i + 1u < sizeof(g_state.reason) && text[i] != '\0'; i++) {
        g_state.reason[i] = text[i];
    }

    g_state.reason[i] = '\0';
}

static void wifi_8265_set_phase(enum wifi_8265_phase phase, const char *reason)
{
    g_state.phase = phase;
    wifi_8265_reason(reason);
    if (reason != NULL && reason[0] != '\0') {
        klog(K_LOG_LEVEL_INFO, "wifi: state %s: %s", wifi_8265_phase_name(phase), reason);
    } else {
        klog(K_LOG_LEVEL_INFO, "wifi: state %s", wifi_8265_phase_name(phase));
    }
}

static int wifi_8265_fail(int error, const char *reason)
{
    g_state.error = error;
    wifi_8265_set_phase(WIFI_8265_PHASE_FAILED, reason);
    return error;
}

/* --- the register file ----------------------------------------------------- */

static bool wifi_8265_window_has(k_u32_t offset)
{
    return g_regs != NULL && g_window_size >= 4u && offset <= g_window_size - 4u &&
           (offset & 3u) == 0u;
}

k_u32_t iwl8265_csr_read(k_u32_t offset)
{
    if (!wifi_8265_window_has(offset)) {
        return 0xFFFFFFFFu;
    }

    return g_regs[offset / 4u];
}

void iwl8265_csr_write(k_u32_t offset, k_u32_t value)
{
    if (!wifi_8265_window_has(offset)) {
        return;
    }

    g_regs[offset / 4u] = value;
}

/*
 * An internal register is reached by writing its address to the HBUS door
 * and then reading or writing the data word. The mask of an 8000-family
 * card is twenty bits, and the upper two bits of the address word ask for a
 * four-byte access; both are the numbers of iwl_trans_pcie_read_prph().
 */
k_u32_t iwl8265_prph_read(k_u32_t offset)
{
    iwl8265_csr_write(IWL8265_HBUS_TARG_PRPH_RADDR,
                      (offset & IWL8265_PRPH_ADDR_MASK) | IWL8265_PRPH_DWORD);
    return iwl8265_csr_read(IWL8265_HBUS_TARG_PRPH_RDAT);
}

void iwl8265_prph_write(k_u32_t offset, k_u32_t value)
{
    iwl8265_csr_write(IWL8265_HBUS_TARG_PRPH_WADDR,
                      (offset & IWL8265_PRPH_ADDR_MASK) | IWL8265_PRPH_DWORD);
    iwl8265_csr_write(IWL8265_HBUS_TARG_PRPH_WDAT, value);
}

/* --- the clock of the waits ------------------------------------------------ */

static k_u32_t wifi_8265_mmio_cb(k_u32_t offset, void *opaque)
{
    K_UNUSED(opaque);
    return iwl8265_csr_read(offset);
}

static void wifi_8265_mmio_write_cb(k_u32_t offset, k_u32_t value, void *opaque)
{
    K_UNUSED(opaque);
    iwl8265_csr_write(offset, value);
}

static k_u64_t wifi_8265_ticks_cb(void *opaque)
{
    K_UNUSED(opaque);
    return wifi_test_ticks();
}

static const struct iwl8265_wait_io g_wait_io = {
    wifi_8265_mmio_cb,
    wifi_8265_mmio_write_cb,
    wifi_8265_ticks_cb,
    NULL,
};

/*
 * One bounded wait of the probe. The log names the register, the last word
 * read and the milliseconds spent, so a card that does not answer says
 * exactly where it stopped. The value is kept for the state as well.
 */
static bool wifi_8265_wait(const struct iwl8265_wait_io *io, k_u32_t reg, const char *name,
                           k_u32_t mask, k_u32_t expect, k_u32_t budget_ms, k_u32_t *value)
{
    k_u32_t last = 0u;
    k_u32_t spent = 0u;

    if (!iwl8265_wait_bits(io, reg, mask, expect, budget_ms, &last, &spent)) {
        kerror("wifi: timeout waiting for %s (CSR 0x%03x 0x%08x after %u ms)", name, (unsigned)reg,
               (unsigned)last, (unsigned)spent);

        if (value != NULL) {
            *value = last;
        }

        return false;
    }

    if (value != NULL) {
        *value = last;
    }

    return true;
}

/* A card coming out of D3hot needs a short settle before its window answers
 * (PCI PM 1.2 §5.6.1). The guard is for a machine whose PIT has not started
 * yet: a boot must not hang on a clock that never moves. */
static void wifi_8265_delay_ms(k_u32_t ms)
{
    k_u64_t start = wifi_test_ticks();
    k_u32_t guard;

    for (guard = 0u; guard < ms * 100000u && wifi_test_ticks() - start < ms; guard++) {
        __asm__ volatile("pause");
    }
}

/* --- attach ---------------------------------------------------------------- */

static void wifi_8265_power_d0(const struct port_x86_64_pci_device *card)
{
    k_u16_t status = port_x86_64_pci_read16(card, WIFI_8265_PCI_STATUS);
    k_u8_t pointer;
    k_u8_t guard;

    if ((status & WIFI_8265_PCI_STATUS_CAP_LIST) == 0u) {
        return;
    }

    pointer = (k_u8_t)(port_x86_64_pci_read32(card, WIFI_8265_PCI_CAP_POINTER) & 0xFCu);

    for (guard = 0u; guard < WIFI_8265_PCI_CAP_GUARD && pointer >= 0x40u; guard++) {
        k_u32_t head = port_x86_64_pci_read32(card, pointer);

        if ((k_u8_t)(head & 0xFFu) == WIFI_8265_PCI_CAP_ID_POWER) {
            k_u8_t csr_offset = (k_u8_t)(pointer + 4u);
            k_u16_t csr = port_x86_64_pci_read16(card, csr_offset);

            /* Only the two power-state bits change; the other bits of the
             * PMCSR belong to the firmware and to the wake configuration. */
            if ((csr & WIFI_8265_PCI_PMCSR_POWER_MASK) != 0u) {
                port_x86_64_pci_write16(card, csr_offset,
                                        (k_u16_t)(csr & (k_u16_t)~WIFI_8265_PCI_PMCSR_POWER_MASK));
                wifi_8265_delay_ms(WIFI_8265_D0_SETTLE_MS);
            }

            return;
        }

        pointer = (k_u8_t)((head >> 8) & 0xFCu);
    }
}

int wifi_8265_attach(void)
{
    struct port_x86_64_pci_device card;
    k_u32_t bar0;
    k_u32_t bar1 = 0u;
    k_u32_t offset;
    k_u32_t span;
    k_u64_t phys;
    k_u16_t command;

    g_regs = NULL;
    g_window_size = 0u;
    g_state = (struct wifi_8265_state){0};

    if (!port_x86_64_pci_find(WIFI_8265_VENDOR_INTEL, WIFI_8265_DEVICE, &card)) {
        /* No card is a machine without this driver, not a boot error. */
        return 0;
    }

    g_state.present = true;
    g_state.vendor = port_x86_64_pci_read16(&card, PCI_VENDOR);
    g_state.device = port_x86_64_pci_read16(&card, PCI_DEVICE);
    g_state.revision = (k_u8_t)(port_x86_64_pci_read16(&card, PCI_REVISION) & 0xFFu);
    g_state.bus = card.bus;
    g_state.slot = card.slot;
    g_state.function = card.function;

    wifi_8265_power_d0(&card);

    /* Enable the memory window and bus mastering without throwing away the
     * bits the firmware set, and clear the command's interrupt disable: the
     * device is still masked at CSR_INT_MASK and no MSI is enabled. */
    command = port_x86_64_pci_read16(&card, PCI_COMMAND);
    command = (k_u16_t)((command | PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER) &
                        (k_u16_t)~WIFI_8265_PCI_COMMAND_INT_DISABLE);
    port_x86_64_pci_write16(&card, PCI_COMMAND, command);

    bar0 = port_x86_64_pci_read32(&card, PCI_BAR0);

    if ((bar0 & PCI_BAR_IO) != 0u) {
        return wifi_8265_fail(KERR_INVAL, "BAR0 is an I/O window");
    }

    if (((bar0 >> 1) & 3u) == 2u) {
        bar1 = port_x86_64_pci_read32(&card, PCI_BAR1);
    }

    phys = ((k_u64_t)bar1 << 32) | (k_u64_t)(bar0 & ~0x0Fu);
    g_state.bar0 = phys;
    g_state.bar0_size = port_x86_64_pci_bar_size(&card, PCI_BAR0);

    if (g_state.bar0_size < WIFI_8265_MIN_BAR_SIZE) {
        return wifi_8265_fail(KERR_INVAL, "BAR0 is smaller than 0x2000");
    }

    if (phys == 0u) {
        return wifi_8265_fail(KERR_INVAL, "BAR0 names no window");
    }

    /* The map is in whole 2 MiB pages, so the card sits at the offset of its
     * window inside the first one. The same virtual base is used by every
     * address space of the port. */
    offset = (k_u32_t)phys & WIFI_8265_LARGE_PAGE_MASK;
    span = offset + g_state.bar0_size;

    if (port_x86_64_paging_map_mmio(PORT_X86_64_WIFI_VIRT, phys - offset, (k_size_t)span) == 0u) {
        return wifi_8265_fail(KERR_IO, "BAR0 is not in the map of the port");
    }

    g_regs = (volatile k_u32_t *)(uintptr_t)(PORT_X86_64_WIFI_VIRT + offset);
    g_window_size = g_state.bar0_size;

    if (!port_x86_64_paging_kernel_present((k_u64_t)(uintptr_t)g_regs &
                                           ~WIFI_8265_LARGE_PAGE_MASK)) {
        g_regs = NULL;
        return wifi_8265_fail(KERR_IO, "BAR0 translation is not present");
    }

    /* A window that answers all ones is a window of nothing. */
    g_state.hw_rev = iwl8265_csr_read(IWL8265_CSR_HW_REV);
    g_state.hw_if_config = iwl8265_csr_read(IWL8265_CSR_HW_IF_CONFIG);

    if (g_state.hw_rev == 0xFFFFFFFFu || g_state.hw_if_config == 0xFFFFFFFFu) {
        g_regs = NULL;
        return wifi_8265_fail(KERR_IO, "BAR0 answers all ones");
    }

    /* Status and control words only: no EEPROM read command, no OTP write. */
    g_state.eeprom = iwl8265_csr_read(IWL8265_CSR_EEPROM_REG);
    g_state.eeprom_gp = iwl8265_csr_read(IWL8265_CSR_EEPROM_GP);
    g_state.otp_gp = iwl8265_csr_read(IWL8265_CSR_OTP_GP_REG);
    g_state.gp_cntrl = iwl8265_csr_read(IWL8265_CSR_GP_CNTRL);
    g_state.hw_type = iwl8265_decode_hw_type(g_state.hw_rev);
    g_state.hw_step = iwl8265_decode_hw_step(g_state.hw_rev);

    /* The port has no handler for a card it cannot answer yet. Acknowledge
     * whatever the firmware left pending and keep every source masked. */
    iwl8265_csr_write(IWL8265_CSR_INT_MASK, 0u);
    iwl8265_csr_write(IWL8265_CSR_INT, 0xFFFFFFFFu);
    iwl8265_csr_write(IWL8265_CSR_FH_INT_STATUS, 0xFFFFFFFFu);

    g_state.error = KERR_NONE;
    wifi_8265_set_phase(WIFI_8265_PHASE_ATTACHED, "PCI function mapped");
    kinfo("wifi: Intel 8265 (%04x:%04x rev %02x) at %02x:%02x.%x, CSR_HW_REV 0x%08x",
          (unsigned)g_state.vendor, (unsigned)g_state.device, (unsigned)g_state.revision,
          (unsigned)g_state.bus, (unsigned)g_state.slot, (unsigned)g_state.function,
          (unsigned)g_state.hw_rev);

    return 0;
}

/* --- the bounded power probe ----------------------------------------------- */

static void wifi_8265_mmio_write(const struct iwl8265_wait_io *io, k_u32_t offset, k_u32_t value)
{
    if (io != NULL && io->write != NULL) {
        io->write(offset, value, io->opaque);
    }
}

int wifi_8265_power_sequence(const struct iwl8265_wait_io *io, k_u32_t *hw_config, k_u32_t *gp)
{
    k_u32_t value;

    /* The 8265 has no APMG: these CSR steps are the basic init before the
     * ownership handshake. Every write is a read-modify-write. */
    value = io->read(IWL8265_CSR_DBG_LINK_PWR_MGMT_REG, io->opaque);
    wifi_8265_mmio_write(io, IWL8265_CSR_DBG_LINK_PWR_MGMT_REG,
                         value | IWL8265_LINK_PWR_MGMT_DISABLED);

    value = io->read(IWL8265_CSR_GIO_CHICKEN_BITS, io->opaque);
    wifi_8265_mmio_write(io, IWL8265_CSR_GIO_CHICKEN_BITS,
                         value | IWL8265_GIO_CHICKEN_L1A_NO_L0S_RX);

    value = io->read(IWL8265_CSR_DBG_HPET_MEM_REG, io->opaque);
    wifi_8265_mmio_write(io, IWL8265_CSR_DBG_HPET_MEM_REG, value | IWL8265_DBG_HPET_MEM_VALUE);

    value = io->read(IWL8265_CSR_HW_IF_CONFIG, io->opaque);
    wifi_8265_mmio_write(io, IWL8265_CSR_HW_IF_CONFIG, value | IWL8265_HW_IF_HAP_WAKE_L1A);
    wifi_8265_mmio_write(io, IWL8265_CSR_HW_IF_CONFIG,
                         value | IWL8265_HW_IF_HAP_WAKE_L1A | IWL8265_HW_IF_NIC_READY);

    if (!wifi_8265_wait(io, IWL8265_CSR_HW_IF_CONFIG, "NIC_READY", IWL8265_HW_IF_NIC_READY,
                        IWL8265_HW_IF_NIC_READY, WIFI_8265_HW_READY_MS, &value)) {
        return KERR_IO;
    }
    wifi_8265_mmio_write(io, IWL8265_CSR_MBOX_SET_REG, IWL8265_MBOX_SET_OS_ALIVE);

    value = io->read(IWL8265_CSR_GP_CNTRL, io->opaque);
    wifi_8265_mmio_write(io, IWL8265_CSR_GP_CNTRL, value | IWL8265_GP_MAC_ACCESS_REQ);
    if (!wifi_8265_wait(io, IWL8265_CSR_GP_CNTRL, "MAC_CLOCK_READY",
                        IWL8265_GP_CLOCK_READY | IWL8265_GP_GOING_TO_SLEEP, IWL8265_GP_CLOCK_READY,
                        WIFI_8265_MAC_ACCESS_MS, &value)) {
        return KERR_IO;
    }

    value = io->read(IWL8265_CSR_GP_CNTRL, io->opaque);
    wifi_8265_mmio_write(io, IWL8265_CSR_GP_CNTRL, value | IWL8265_GP_INIT_DONE);
    if (!wifi_8265_wait(io, IWL8265_CSR_GP_CNTRL, "INIT_DONE clock", IWL8265_GP_CLOCK_READY,
                        IWL8265_GP_CLOCK_READY, WIFI_8265_CLOCK_READY_MS, &value)) {
        return KERR_IO;
    }

    value = io->read(IWL8265_CSR_HW_IF_CONFIG, io->opaque);
    wifi_8265_mmio_write(io, IWL8265_CSR_HW_IF_CONFIG, value | IWL8265_HW_IF_PERSIST_MODE);
    *hw_config = io->read(IWL8265_CSR_HW_IF_CONFIG, io->opaque);
    *gp = io->read(IWL8265_CSR_GP_CNTRL, io->opaque);
    return KERR_NONE;
}

int wifi_8265_power_up(void)
{
    k_u32_t hw_config;
    k_u32_t gp;
    int error;

    if (!g_state.present) {
        return 0;
    }
    if (g_state.phase != WIFI_8265_PHASE_ATTACHED) {
        return g_state.phase == WIFI_8265_PHASE_FAILED ? g_state.error : KERR_INVAL;
    }

    error = wifi_8265_power_sequence(&g_wait_io, &hw_config, &gp);
    if (error != KERR_NONE) {
        return wifi_8265_fail(error,
                              g_state.reason[0] != '\0' ? g_state.reason : "power probe failed");
    }

    g_state.hw_if_config = hw_config;
    g_state.gp_cntrl = gp;
    g_state.error = KERR_NONE;
    wifi_8265_set_phase(WIFI_8265_PHASE_POWER_UP, "MAC clock ready");
    return 0;
}
