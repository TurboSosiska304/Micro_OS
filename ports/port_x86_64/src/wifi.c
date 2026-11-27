/*
 * Micro OS — x86-64 Intel 8265 discovery, firmware and health reporting.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/iwl-csr.h, fw/file.h and
 * pcie/trans.c (GPL-2.0, Linux v6.12); code written here (MIT).
 *
 * Discovery is read-only apart from the PCI/CSR power steps in wifi_8265.c.
 * Loading firmware and waiting for ALIVE are separate, explicit operations:
 * the boot and the default `wifi start` only report a dry run. No scan,
 * association or frame transmission is implemented in this slice.
 */
#include "port_x86_64_wifi.h"

#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kshell/kshell.h"
#include "wifi/iwl8265_fw.h"
#include "wifi/iwl8265_fw_file.h"
#include "wifi/iwl8265_loader.h"

static kmod_state_t g_wifi_status = KMOD_SKIP;
static char g_wifi_info[KMOD_INFO_MAX] = "no Intel 8265 found";
static bool g_wifi_registered;
static struct wifi_iwl8265_fw_file g_firmware_file;
static struct wifi_iwl8265_fw g_firmware;
static bool g_firmware_parsed;
static struct wifi_iwl8265_alive g_alive;
static const char *g_firmware_error = "firmware file cannot be read";

static void wifi_set_kmod(kmod_state_t state, const char *info)
{
    k_size_t i;

    g_wifi_status = state;
    if (info == NULL) {
        g_wifi_info[0] = '\0';
    } else {
        snprintf(g_wifi_info, sizeof(g_wifi_info), "%s", info);
    }

    for (i = 0u; i < kmod_count(); i++) {
        const struct kmod *mod = kmod_get(i);
        if (mod != NULL && mod->name != NULL && strcmp(mod->name, "wifi") == 0) {
            struct kmod *writable = (struct kmod *)mod;
            writable->state = state;
            snprintf(writable->info, sizeof(writable->info), "%s", g_wifi_info);
            break;
        }
    }
}

static int wifi_check(void)
{
    if (g_wifi_status == KMOD_OK) {
        return 0;
    }
    if (g_wifi_status == KMOD_DEGRADED) {
        return 1;
    }
    if (g_wifi_status == KMOD_SKIP) {
        return 2;
    }
    return -1;
}

static const char *wifi_detail(void)
{
    return g_wifi_info;
}

static int wifi_mod_init(void)
{
    const struct wifi_8265_state *state;
    int rc = wifi_8265_attach();

    if (rc != 0) {
        wifi_set_kmod(KMOD_FAIL, "PCI attach failed");
        return rc;
    }

    state = wifi_8265_state();
    if (!state->present) {
        wifi_set_kmod(KMOD_SKIP, "no Intel 8265 found");
        return 0;
    }

    rc = wifi_8265_power_up();
    if (rc != 0) {
        wifi_set_kmod(KMOD_FAIL, state->reason);
        return rc;
    }

    wifi_set_kmod(KMOD_SKIP, "card powered; firmware not loaded");
    return 0;
}

static const struct kmod g_wifi_mod = {
    .name = "wifi",
    .desc = "Intel 8265 PCIe radio",
    .init = wifi_mod_init,
    .check = wifi_check,
    .detail = wifi_detail,
};

/* --- firmware file and parser ------------------------------------------------ */

static const char *wifi_fw_error(enum wifi_iwl8265_fw_result result)
{
    switch (result) {
    case WIFI_IWL8265_FW_UNSUPPORTED:
        return "unsupported firmware API";
    case WIFI_IWL8265_FW_NOSPACE:
        return "too many firmware sections";
    default:
        return "invalid firmware container";
    }
}

static int wifi_read_firmware(void)
{
    enum wifi_iwl8265_fw_file_result file_result;
    enum wifi_iwl8265_fw_result parse_result;

    if (g_firmware_file.data != NULL) {
        kmm_free(g_firmware_file.data);
        memset(&g_firmware_file, 0, sizeof(g_firmware_file));
    }
    g_firmware_parsed = false;
    g_firmware_error = "firmware file cannot be read";

    file_result = wifi_iwl8265_fw_file_load(&g_firmware_file);
    if (file_result == WIFI_IWL8265_FW_FILE_NONE) {
        wifi_set_kmod(KMOD_SKIP, "no iwlwifi-8265-22..36.ucode file");
        klog(K_LOG_LEVEL_INFO, "wifi: no iwlwifi-8265-22..36.ucode file, firmware load skipped");
        return 2;
    }
    if (file_result != WIFI_IWL8265_FW_FILE_OK) {
        const char *reason = "firmware file cannot be read";

        if (file_result == WIFI_IWL8265_FW_FILE_NOMEM) {
            reason = "no memory for firmware file";
        } else if (file_result == WIFI_IWL8265_FW_FILE_TOO_BIG) {
            reason = "firmware file is too large";
        }
        g_firmware_error = reason;
        wifi_set_kmod(KMOD_FAIL, reason);
        return -1;
    }

    parse_result = wifi_iwl8265_fw_parse(g_firmware_file.data, g_firmware_file.size, &g_firmware);
    if (parse_result == WIFI_IWL8265_FW_UNSUPPORTED) {
        wifi_set_kmod(KMOD_SKIP, "firmware API is outside 22..36");
        return 2;
    }
    if (parse_result != WIFI_IWL8265_FW_OK) {
        g_firmware_error = wifi_fw_error(parse_result);
        wifi_set_kmod(KMOD_FAIL, g_firmware_error);
        return -1;
    }
    g_firmware_parsed = true;
    return 0;
}

/* --- x86-64 operations for the portable loader ------------------------------- */

static void *wifi_dma_alloc(void *context, k_size_t size, k_size_t alignment, k_u64_t *phys)
{
    (void)context;
    (void)alignment;
    *phys = 0u;
    return kmm_alloc(size, KMM_PRIORITY_KERNEL);
}

static void wifi_dma_free(void *context, void *buffer, k_size_t size)
{
    (void)context;
    (void)size;
    kmm_free(buffer);
}

static k_u32_t wifi_loader_read_prph(void *context, k_u32_t offset)
{
    (void)context;
    return iwl8265_prph_read(offset);
}

static void wifi_loader_write_prph(void *context, k_u32_t offset, k_u32_t value)
{
    (void)context;
    iwl8265_prph_write(offset, value);
}

static k_u64_t wifi_loader_phys(void *context, const void *buffer)
{
    (void)context;
    return port_x86_64_dma_phys(buffer);
}

static k_u64_t wifi_loader_now(void *context)
{
    (void)context;
    return port_x86_64_ticks();
}

/* The wrappers have the callback shape; keeping them separate makes it clear
 * that the portable loader never includes an x86 header. */
static k_u32_t wifi_loader_read(void *context, k_u32_t offset)
{
    (void)context;
    return iwl8265_csr_read(offset);
}

static void wifi_loader_write(void *context, k_u32_t offset, k_u32_t value)
{
    (void)context;
    iwl8265_csr_write(offset, value);
}

static void wifi_loader_write64(void *context, k_u32_t offset, k_u64_t value)
{
    (void)context;
    iwl8265_csr_write(offset, (k_u32_t)value);
    iwl8265_csr_write(offset + 4u, (k_u32_t)(value >> 32));
}

static const struct wifi_iwl8265_load_ops g_load_ops = {
    NULL,
    wifi_loader_read,
    wifi_loader_write,
    wifi_loader_write64,
    wifi_loader_read_prph,
    wifi_loader_write_prph,
    wifi_loader_phys,
    wifi_loader_now,
    wifi_dma_alloc,
    wifi_dma_free,
};

static void wifi_print_info(void)
{
    const struct wifi_8265_state *state = wifi_8265_state();
    const char *version = g_firmware_parsed ? g_firmware.version : "none";

    kshell_printf("wifi: ID 8086:24fd rev %02x at %02x:%02x.%x\n", (unsigned)state->revision,
                  (unsigned)state->bus, (unsigned)state->slot, (unsigned)state->function);
    kshell_printf("wifi: CSR_HW_REV 0x%08x, HW_IF_CONFIG 0x%08x\n", (unsigned)state->hw_rev,
                  (unsigned)state->hw_if_config);
    kshell_printf("wifi: EEPROM 0x%08x, EEPROM_GP 0x%08x, OTP_GP 0x%08x\n", (unsigned)state->eeprom,
                  (unsigned)state->eeprom_gp, (unsigned)state->otp_gp);
    kshell_printf("wifi: BAR0 0x%x%08x+0x%x\n", (unsigned)(state->bar0 >> 32),
                  (unsigned)state->bar0, (unsigned)state->bar0_size);
    kshell_printf("wifi: firmware API %u build %u, %s\n",
                  g_firmware_parsed ? (unsigned)g_firmware.api : 0u,
                  g_firmware_parsed ? (unsigned)g_firmware.build : 0u, version);
    kshell_printf("wifi: phase %s, module %s, %s\n", wifi_8265_phase_name(state->phase),
                  kmod_state_name(g_wifi_status), g_wifi_info);
}

static int wifi_start(bool confirmed)
{
    const struct wifi_8265_state *state = wifi_8265_state();
    int rc;

    if (!state->present) {
        kshell_printf("wifi: no Intel 8265, nothing to start\n");
        return KSHELL_OK;
    }
    if (!confirmed) {
        kshell_printf("wifi: dry run: would power the card and load iwlwifi-8265 firmware; "
                      "use wifi start --yes\n");
        return KSHELL_OK;
    }
    if (state->phase == WIFI_8265_PHASE_ALIVE) {
        kshell_printf("wifi: firmware is already alive\n");
        return KSHELL_OK;
    }
    if (state->phase == WIFI_8265_PHASE_FAILED) {
        kshell_printf("wifi: the card is in a failed state: %s\n", state->reason);
        return KSHELL_ERROR;
    }

    rc = wifi_read_firmware();
    if (rc == 2) {
        kshell_printf("wifi: skipped: install iwlwifi-8265-36.ucode (API 22..36) in "
                      "/etc/firmware\n");
        return KSHELL_OK;
    }
    if (rc != 0) {
        kshell_printf("wifi: %s\n", g_firmware_error);
        return KSHELL_ERROR;
    }

    if (state->phase != WIFI_8265_PHASE_POWERED) {
        kshell_printf("wifi: card phase is %s, cannot load firmware\n",
                      wifi_8265_phase_name(state->phase));
        return KSHELL_ERROR;
    }

    rc = wifi_iwl8265_load(&g_firmware, &g_load_ops, &g_alive);
    if (rc != 0) {
        wifi_set_kmod(KMOD_FAIL, "firmware load or ALIVE failed");
        kshell_printf("wifi: firmware load failed; see the timeout/error line above\n");
        return KSHELL_ERROR;
    }

    /* The transport owns this snapshot; no radio operation is exposed yet. */
    {
        struct wifi_8265_state *live = (struct wifi_8265_state *)wifi_8265_state();
        live->phase = WIFI_8265_PHASE_ALIVE;
        live->error = KERR_NONE;
        live->reason[0] = '\0';
    }
    wifi_set_kmod(KMOD_DEGRADED, "firmware alive; no scan/association backend");
    kshell_printf("wifi: ALIVE status 0x%04x, LMAC %u.%u, UMAC %u.%u, SCD 0x%08x\n",
                  (unsigned)g_alive.status, (unsigned)g_alive.lmac_major,
                  (unsigned)g_alive.lmac_minor, (unsigned)g_alive.umac_major,
                  (unsigned)g_alive.umac_minor, (unsigned)g_alive.scd_base);
    return KSHELL_OK;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        wifi_print_info();
        return g_wifi_status == KMOD_FAIL ? KSHELL_ERROR : KSHELL_OK;
    }
    if (argc == 2 && strcmp(argv[1], "start") == 0) {
        return wifi_start(false);
    }
    if (argc == 3 && strcmp(argv[1], "start") == 0 && strcmp(argv[2], "--yes") == 0) {
        return wifi_start(true);
    }
    kshell_printf("usage: wifi [info|start [--yes]]\n");
    return KSHELL_ERROR;
}

static bool g_command_registered;

void port_x86_64_wifi_init(void)
{
    if (!g_wifi_registered && kmod_register(&g_wifi_mod) == 0) {
        g_wifi_registered = true;
    }
    if (!g_command_registered &&
        kshell_register("wifi", "Intel 8265 radio status", cmd_wifi) == 0) {
        g_command_registered = true;
    }
}

const char *port_x86_64_wifi_kmod_state_name(void)
{
    return kmod_state_name(g_wifi_status);
}
