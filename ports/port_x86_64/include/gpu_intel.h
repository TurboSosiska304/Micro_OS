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

#include "kernel/kmod.h"
#include "kernel/types.h"

#include "port_x86_64.h"

/*
 * One uncached 2 MiB window above the other devices of the port. The probe
 * maps the page that contains BAR0 and adds BAR0's offset inside that page.
 */
#define PORT_X86_64_GPU_INTEL_VIRT (PORT_X86_64_KERNEL_BASE + 0x3A000000ull)

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
};

/* What the card and the firmware-left display state answered. */
struct gpu_intel_info {
    const char *name;
    k_u16_t vendor;
    k_u16_t device;
    k_u16_t subsystem_vendor;
    k_u16_t subsystem_device;
    k_u8_t revision;
    k_u8_t bus;
    k_u8_t slot;
    k_u8_t function;
    k_u8_t display_gen;
    k_u8_t display_rel;
    k_u8_t pipes;
    k_u8_t transcoders;
    k_u8_t ports;
    k_u64_t mmio_base;
    k_u32_t fuse_status;
    k_u32_t power_well_bios;
    k_u32_t power_well_driver;
    k_u32_t de_port_isr;
    k_u32_t de_misc_isr;
    k_u32_t edp_ctl;
    k_u16_t panel_width;
    k_u16_t panel_height;
    k_u8_t panel_pipe; /* 0xff when no active eDP timing was read */
    bool mmio_responded;
    bool display_ready;
};

/* The identity table is deliberately testable without a PCI bus. */
const struct gpu_intel_id *gpu_intel_id_lookup(k_u16_t vendor, k_u16_t device);

/*
 * Find a supported card and perform the read-only handshake. No card is a
 * skip, not a boot failure. The future gpu.c dispatcher owns registration and
 * decides how to expose the returned state and information.
 */
kmod_state_t gpu_intel_probe(struct gpu_intel_info *info);

#endif /* MICRO_OS_PORT_X86_64_GPU_INTEL_H */
