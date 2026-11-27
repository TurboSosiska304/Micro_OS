/*
 * Micro OS — firmware load plan for the Intel 8265.
 *
 * Facts from drivers/net/wireless/intel/iwlwifi/pcie/trans.c,
 * pcie/ctxt-info.c, iwl-context-info.h, iwl-fh.h and iwl-prph.h
 * (GPL-2.0, Linux v6.12); code written here (MIT).
 *
 * This is the original Micro OS implementation of the documented register
 * protocol. It deliberately has no Linux types, allocation model or code.
 */
#ifndef MICRO_OS_WIFI_IWL8265_LOADER_H
#define MICRO_OS_WIFI_IWL8265_LOADER_H

#include "kernel/types.h"

#include "wifi/iwl8265_fw.h"

#define WIFI_IWL8265_LOAD_WAIT_MS 5000u
#define WIFI_IWL8265_ALIVE_WAIT_MS 10000u
#define WIFI_IWL8265_CONTEXT_BYTES (208u + 3u * 64u * 8u + 64u)

struct wifi_iwl8265_alive {
    k_u16_t status;
    k_u16_t flags;
    k_u32_t lmac_major;
    k_u32_t lmac_minor;
    k_u32_t umac_major;
    k_u32_t umac_minor;
    k_u32_t scd_base;
    bool valid;
};

/* Hardware access used by the loader; the x86-64 port supplies it. */
struct wifi_iwl8265_load_ops {
    void *context;
    k_u32_t (*read)(void *context, k_u32_t offset);
    void (*write)(void *context, k_u32_t offset, k_u32_t value);
    void (*write64)(void *context, k_u32_t offset, k_u64_t value);
    k_u32_t (*read_prph)(void *context, k_u32_t offset);
    void (*write_prph)(void *context, k_u32_t offset, k_u32_t value);
    k_u64_t (*dma_phys)(void *context, const void *buffer);
    k_u64_t (*now_ms)(void *context);
    void *(*alloc)(void *context, k_size_t size, k_size_t alignment, k_u64_t *phys);
    void (*free)(void *context, void *buffer, k_size_t size);
};

/*
 * Load all image sections, paging blocks and the context descriptor, then
 * wait for ALIVE. The firmware buffer and ops must remain valid for the call.
 * No scan, association, NVM write or radio state change is performed.
 */
int wifi_iwl8265_load(const struct wifi_iwl8265_fw *firmware,
                      const struct wifi_iwl8265_load_ops *ops, struct wifi_iwl8265_alive *alive);

#endif /* MICRO_OS_WIFI_IWL8265_LOADER_H */
