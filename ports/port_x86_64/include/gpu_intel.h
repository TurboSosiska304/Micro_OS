/*
 * Micro OS — Intel-specific GPU details over the shared x86-64 GPU contract.
 *
 * Facts from drivers/gpu/drm/i915/i915_reg.h,
 * drivers/gpu/drm/i915/gt/intel_gtt.h and
 * drivers/gpu/drm/i915/display/skl_universal_plane.c (GPL-2.0, Linux v6.12);
 * code written here for Micro OS (MIT).
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_INTEL_H
#define MICRO_OS_PORT_X86_64_GPU_INTEL_H

#include "gpu_intel_ggtt.h"
#include "port_x86_64_gpu.h"

/*
 * One uncached 16 MiB MMIO window above the other devices of the port. The
 * first 8 MiB holds registers; the second half is the gen8 GGTT alias. BAR2,
 * the 256 MiB graphics aperture, is mapped separately when a later stage needs
 * to access the pages behind those entries.
 */
#define PORT_X86_64_GPU_INTEL_VIRT (PORT_X86_64_KERNEL_BASE + 0x3B000000ull)

/* BAR2 is mapped write-combining only while a GGTT aperture test needs it. */
#define PORT_X86_64_GPU_INTEL_APERTURE_VIRT (PORT_X86_64_KERNEL_BASE + 0x3C000000ull)

/* Intel-only observations which do not belong in the shared vendor handshake. */
struct gpu_intel_info {
    struct gpu_info common;
    k_u16_t subsystem_vendor;
    k_u16_t subsystem_device;
    k_u8_t pipes;
    k_u8_t transcoders;
    k_u8_t ports;
    k_u32_t fuse_status;
    k_u32_t power_well_bios;
    k_u32_t power_well_driver;
    k_u32_t de_port_isr;
    k_u32_t de_misc_isr;
    k_u32_t edp_ctl;
    k_u8_t panel_pipe; /* 0xff when no active eDP timing was read */
    bool mmio_responded;
    bool display_ready;

    /* Read-only stage 1: the firmware-left primary plane and its GGTT frame. */
    k_u64_t gtt_aperture_base;
    k_u64_t gtt_aperture_size;
    k_u32_t plane_control;
    k_u32_t plane_stride;
    k_u32_t plane_surface;
    struct gpu_intel_fb framebuffer;
    bool framebuffer_known;
};

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

/* The shared dispatcher supplies the PCI function and receives the common result. */
enum gpu_probe_result gpu_intel_probe(const struct port_x86_64_pci_device *pci,
                                      struct gpu_info *out);

/* Optional subcommands for the shared `gpu` command. */
int gpu_intel_command(int argc, char **argv);

#endif /* MICRO_OS_PORT_X86_64_GPU_INTEL_H */
