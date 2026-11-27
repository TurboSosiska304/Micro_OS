/*
 * Micro OS — the x86-64 port: the Intel 8265 on the PCI bus of a laptop.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/iwl-csr.h and pcie/trans.c
 * (GPL-2.0, Linux v6.12); code written here (MIT).
 *
 * The attach reads the identity of the card and makes its register window
 * usable; the power probe takes the card out of its low-power state without
 * loading a firmware. A machine without the function is not an error: the
 * state stays present=false in phase NONE and both calls return at once.
 *
 * The register facts, the two decoders and the bounded-wait primitive are
 * inline in iwl8265_regs.h, so a host test can check them without a card.
 * Only the MMIO and the PCI walk of wifi_8265.c need the machine.
 */
/* clang-format off */
/*
Facts from drivers/net/wireless/intel/iwlwifi/... (GPL-2.0, Linux v6.12); code written here (MIT).
*/
/* clang-format on */
#ifndef MICRO_OS_PORT_X86_64_WIFI_H
#define MICRO_OS_PORT_X86_64_WIFI_H

#include "kernel/types.h"

#include "port_x86_64.h"

#include "iwl8265_regs.h"

/*
 * The window of BAR0 in the map of the kernel: above the screen (0x20000000),
 * the APIC (0x30000000), USB (0x32000000), SATA (0x34000000), NVMe
 * (0x36000000), ethernet (0x38000000), the GPU (0x3A000000) and its aperture
 * (0x3C000000), inside the 1 GiB the table of the port maps for every task.
 */
#define PORT_X86_64_WIFI_VIRT (PORT_X86_64_KERNEL_BASE + 0x3D000000ull)

/* The steps a probe goes through, in the order the card answers them. */
enum wifi_8265_phase {
    WIFI_8265_PHASE_NONE = 0,
    WIFI_8265_PHASE_ATTACHED,
    WIFI_8265_PHASE_POWERED,
    WIFI_8265_PHASE_FIRMWARE_READY,
    WIFI_8265_PHASE_ALIVE,
    WIFI_8265_PHASE_FAILED
};

/*
 * What this port knows of the card. The EEPROM/OTP words are status and
 * control only - no read of the non-volatile memory is issued to fill them.
 */
struct wifi_8265_state {
    bool present;
    k_u16_t vendor;
    k_u16_t device;
    k_u8_t revision;
    k_u8_t bus;
    k_u8_t slot;
    k_u8_t function;
    k_u64_t bar0;
    k_u32_t bar0_size;
    k_u32_t hw_rev;
    k_u32_t hw_if_config;
    k_u32_t eeprom;
    k_u32_t eeprom_gp;
    k_u32_t otp_gp;
    k_u32_t gp_cntrl;
    k_u32_t hw_type;
    k_u32_t hw_step;
    enum wifi_8265_phase phase;
    int error;
    char reason[96];
};

/* The state of the card: never NULL, and NONE on a machine without one. */
const struct wifi_8265_state *wifi_8265_state(void);

/* A name for the phase, for the shell and the log; never NULL. */
static inline const char *wifi_8265_phase_name(enum wifi_8265_phase phase)
{
    switch (phase) {
    case WIFI_8265_PHASE_NONE:
        return "none";
    case WIFI_8265_PHASE_ATTACHED:
        return "attached";
    case WIFI_8265_PHASE_POWERED:
        return "powered";
    case WIFI_8265_PHASE_FIRMWARE_READY:
        return "firmware-ready";
    case WIFI_8265_PHASE_ALIVE:
        return "alive";
    case WIFI_8265_PHASE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

/*
 * The step to the next phase when a step of the probe succeeds, or FAILED
 * when it does not. FAILED and ALIVE are terminal: a failed card is not
 * retried by a transition, and a live card has no next step to take.
 */
static inline enum wifi_8265_phase wifi_8265_phase_after(enum wifi_8265_phase phase, bool ok)
{
    if (!ok) {
        return WIFI_8265_PHASE_FAILED;
    }

    switch (phase) {
    case WIFI_8265_PHASE_NONE:
        return WIFI_8265_PHASE_ATTACHED;
    case WIFI_8265_PHASE_ATTACHED:
        return WIFI_8265_PHASE_POWERED;
    case WIFI_8265_PHASE_POWERED:
        return WIFI_8265_PHASE_FIRMWARE_READY;
    case WIFI_8265_PHASE_FIRMWARE_READY:
        return WIFI_8265_PHASE_ALIVE;
    default:
        return phase;
    }
}

/* --- the register file, once BAR0 is mapped -------------------------------- */

/* A read of an unmapped card answers all ones, the same as a card that is
 * not decoding its window: both are a failed probe, not a bogus register. */
k_u32_t iwl8265_csr_read(k_u32_t offset);
void iwl8265_csr_write(k_u32_t offset, k_u32_t value);

/* The internal peripherals are behind the HBUS address/data pair; they need
 * MAC access, which the power probe takes before the first one. */
k_u32_t iwl8265_prph_read(k_u32_t offset);
void iwl8265_prph_write(k_u32_t offset, k_u32_t value);

/* The two decoders are static inline in iwl8265_regs.h. */

/* --- the two steps of the probe -------------------------------------------- */

/* Find the function, set D0, enable its memory, map BAR0, read its identity
 * and mask every device interrupt. No card: phase NONE and a quiet return. */
int wifi_8265_attach(void);

/* Take ownership and clock the MAC with bounded waits; no firmware, no
 * reset. A timeout sets phase FAILED and returns a negative error. */
int wifi_8265_power_up(void);

/* Register the "wifi" kmod and shell command. */
void port_x86_64_wifi_init(void);

/* Run the bounded power sequence against injectable MMIO. This is the same
 * function the port uses; host tests provide a register file and a clock. */
int wifi_8265_power_sequence(const struct iwl8265_wait_io *io, k_u32_t *hw_config, k_u32_t *gp);

#endif /* MICRO_OS_PORT_X86_64_WIFI_H */
