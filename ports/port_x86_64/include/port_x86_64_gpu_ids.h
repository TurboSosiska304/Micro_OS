/*
 * Micro OS — the x86-64 port: AMD GPU PCI ID table.
 *
 * Pure data/lookup, no port dependencies, so host unit tests can verify it.
 * Provenance: PCI device IDs are public assignments; the 1002:67df entry was
 * confirmed on live hardware (Sapphire Nitro+ RX 570, 26 Nov 2026). No Linux
 * source code was copied.
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_IDS_H
#define MICRO_OS_PORT_X86_64_GPU_IDS_H

#include "kernel/types.h"

struct port_x86_64_gpu_amd_id {
    k_u16_t vendor;
    k_u16_t device;
    const char *name;
};

static const struct port_x86_64_gpu_amd_id PORT_X86_64_GPU_AMD_IDS[] = {
    {0x1002u, 0x67DFu, "AMD Radeon RX 470/480/570/570X/580/580X/590"},
    {0x1002u, 0x67C4u, "AMD Radeon RX 470/480"},
    {0x1002u, 0x67C7u, "AMD Radeon RX 580"},
    {0x1002u, 0x67D0u, "AMD Radeon RX 570/580"},
    {0x1002u, 0x67E0u, "AMD Radeon RX 550/560"},
    {0x1002u, 0x67E3u, "AMD Radeon RX 550/560"},
    {0x1002u, 0x67E8u, "AMD Radeon RX 550"},
    {0x1002u, 0x67EBu, "AMD Radeon RX 550"},
    {0x1002u, 0x67EFu, "AMD Radeon RX 460"},
    {0x1002u, 0x67FFu, "AMD Radeon RX 550/560"},
};

#define PORT_X86_64_GPU_AMD_ID_COUNT                                                               \
    (sizeof(PORT_X86_64_GPU_AMD_IDS) / sizeof(PORT_X86_64_GPU_AMD_IDS[0]))

static inline const char *port_x86_64_gpu_amd_lookup(k_u16_t vendor, k_u16_t device)
{
    k_size_t i;

    for (i = 0; i < PORT_X86_64_GPU_AMD_ID_COUNT; i++) {
        if (PORT_X86_64_GPU_AMD_IDS[i].vendor == vendor &&
            PORT_X86_64_GPU_AMD_IDS[i].device == device) {
            return PORT_X86_64_GPU_AMD_IDS[i].name;
        }
    }

    return NULL;
}

#endif /* MICRO_OS_PORT_X86_64_GPU_IDS_H */
