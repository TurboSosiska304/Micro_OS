/*
 * Micro OS — the read-only Intel GPU probe of the x86-64 port.
 *
 * Provenance: PCI identities and Kaby Lake display/register facts were learned
 * from Intel's public hardware documentation and the GPL-licensed Linux i915
 * driver. No i915 source text, macros, control flow or data-structure
 * expression was copied; this implementation and its layout are original.
 *
 * The shared gpu.c/port_x86_64_gpu.h lane is not merged at this base commit.
 * struct gpu_info below is the compatible fallback: after the merge, replace
 * it with the shared declaration and keep the Intel-specific tail in the
 * dispatcher's vendor detail or a mutually agreed extension.
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_INTEL_H
#define MICRO_OS_PORT_X86_64_GPU_INTEL_H

#include "kernel/kmod.h"
#include "kernel/types.h"

#include "port_x86_64.h"

/* A size that was not read is not zero and must not be reported as zero. */
#define GPU_INFO_UNKNOWN_SIZE UINT64_MAX

/*
 * One uncached 2 MiB window above the other devices of the port. The probe
 * maps the page that contains BAR0 and adds BAR0's offset inside that page.
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

#endif /* MICRO_OS_PORT_X86_64_GPU_INTEL_H */
