/*
 * Micro OS — the read-only Intel GPU probe of the x86-64 port.
 *
 * Provenance: PCI identities and Kaby Lake display/register facts were learned
 * from Intel's public hardware documentation and the GPL-licensed Linux i915
 * driver. No i915 source text, macros, control flow or data-structure
 * expression was copied; this implementation and its layout are original.
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_INTEL_H
#define MICRO_OS_PORT_X86_64_GPU_INTEL_H

#include "kernel/types.h"
#include "port_x86_64_gpu.h"

/* A supported PCI identity and the display topology known for that identity. */
struct gpu_intel_id {
    k_u16_t vendor;
    k_u16_t device;
    const char *name;
    k_u8_t display_gen;
    k_u8_t display_rel;
    k_u8_t pipes;
    k_u8_t transcoders;
    k_u8_t ports;
    k_u64_t mmio_size;
};

/* The identity table is deliberately testable without a PCI bus. */
const struct gpu_intel_id *gpu_intel_id_lookup(k_u16_t vendor, k_u16_t device);

#endif /* MICRO_OS_PORT_X86_64_GPU_INTEL_H */
