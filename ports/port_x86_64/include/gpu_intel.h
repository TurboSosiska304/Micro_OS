/*
 * Micro OS — the Intel GPU probe and framebuffer contract of the x86-64 port.
 *
 * Facts from drivers/gpu/drm/i915/i915_reg.h,
 * drivers/gpu/drm/i915/gt/intel_gtt.h and
 * drivers/gpu/drm/i915/display/skl_universal_plane.c (GPL-2.0, Linux v6.12);
 * code written here for Micro OS (MIT).
 *
 * The shared gpu.c/port_x86_64_gpu.h lane is not merged at this base commit.
 * struct gpu_info below is the compatible fallback: after the merge, replace
 * it with the shared declaration and keep the Intel-specific tail in the
 * dispatcher's vendor detail or a mutually agreed extension.
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_INTEL_H
#define MICRO_OS_PORT_X86_64_GPU_INTEL_H

#include "gpu_intel_ggtt.h"
#include "kernel/kmod.h"
#include "kernel/types.h"

#include "port_x86_64.h"

/* A size that was not read is not zero and must not be reported as zero. */
#define GPU_INFO_UNKNOWN_SIZE UINT64_MAX

/*
 * One uncached 16 MiB MMIO window above the other devices of the port. The
 * first 8 MiB holds registers; the second half is the gen8 GGTT alias. BAR2,
 * the 256 MiB graphics aperture, is mapped separately when a later stage needs
 * to access the pages behind those entries.
 */
#define PORT_X86_64_GPU_INTEL_VIRT (PORT_X86_64_KERNEL_BASE + 0x3A000000ull)

/* The common prefix expected by the future gpu.c dispatcher. */
struct gpu_info {
    k_u16_t vendor;
    k_u16_t device;
    k_u8_t rev;
    k_u8_t bus;
    k_u8_t slot;
    k_u8_t func;
    k_u64_t mmio_base;
    k_u64_t mmio_size;
    k_u64_t vram_bytes;
    kmod_state_t state;

    /* Intel detail: the dispatcher may keep this tail or map it to its own. */
    const char *name;
    k_u16_t subsystem_vendor;
    k_u16_t subsystem_device;
    k_u8_t display_gen;
    k_u8_t display_rel;
    const char *display_step;
    k_u8_t pipes;
    k_u8_t transcoders;
    k_u8_t ports;
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

/*
 * int gpu_intel_probe(struct gpu_info *out) is the weak-symbol entry used by
 * the future gpu.c vendor table. Return 0 only when a known card completed the
 * read-only MMIO handshake. Return 1 when no handshake completed (out->state
 * distinguishes KMOD_SKIP, KMOD_UNKNOWN and KMOD_DEGRADED), and -1 on a hard
 * failure. No card is a skip, not a boot failure.
 */
int gpu_intel_probe(struct gpu_info *out);

/* Register the x86-64 `gpu` command after the read-only boot probe. */
int gpu_intel_register(void);

#endif /* MICRO_OS_PORT_X86_64_GPU_INTEL_H */
