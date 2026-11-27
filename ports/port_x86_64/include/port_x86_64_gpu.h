/*
 * Micro OS — the x86-64 port: a display controller found on the PCI bus.
 *
 * The first step of a GPU driver is not drawing: it is to find the card, read
 * the identity it exposes without firmware, and say what answered. This header
 * is the small surface the boot, the shell and vendor probes share: the probe
 * runs once, the report is the same line the log carries, and no display
 * control register is written.
 *
 * Provenance: this interface and its implementation are original work for
 * Micro OS. Hardware facts (PCI IDs, BAR layout, amdgpu discovery log) were
 * observed on a live Sapphire Nitro+ RX 570 (1002:67df rev ef) on 26 Nov 2026;
 * no Linux source code was copied. Register-level facts, when added, must carry
 * a provenance note naming the upstream path/revision and the fact taken, per
 * docs/plan-gpu.md section 5.
 * Facts from drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c (MIT, Linux v6.12).
 * Code written here.
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_H
#define MICRO_OS_PORT_X86_64_GPU_H

#include "kernel/types.h"

#include "port_x86_64_pci.h"

/* The MMIO window of the GPU registers in the top 2 GiB, after the e1000e
 * window. One 2 MiB page is enough for the 256 KiB register BAR of Polaris. */
#define PORT_X86_64_GPU_VIRT (PORT_X86_64_KERNEL_BASE + 0x3A000000ull)
/* One 4 KiB page window into BAR0 for a read-only VRAM probe. */
#define PORT_X86_64_GPU_VRAM_VIRT (PORT_X86_64_KERNEL_BASE + 0x3C000000ull)
/* One fixed, write-combining window for an AMD RGB565/XRGB8888 VRAM surface.
 * It starts after Intel's 256 MiB aperture and ends at the top of the shared
 * high map. */
#define PORT_X86_64_GPU_VRAM_SURFACE_VIRT (PORT_X86_64_KERNEL_BASE + 0x3D000000ull)
#define PORT_X86_64_GPU_VRAM_SURFACE_BYTES (0x40000000ull - 0x3D000000ull)

enum gpu_probe_result {
    GPU_PROBE_NONE = 0,        /* not this vendor's responsibility */
    GPU_PROBE_OK = 1,          /* card identified */
    GPU_PROBE_UNKNOWN_DEV = 2, /* vendor recognized, device unknown */
    GPU_PROBE_DEGRADED = 3,    /* known device, read-only handshake incomplete */
    GPU_PROBE_FAIL = 4,        /* known device, hard probe failure */
};

/* The result of one vendor probe: enough for the boot log and the `gpu`
 * command. All fields are read-only observations; no display state is changed.
 *
 * This struct is the shared handshake surface for vendor probes. The common
 * prefix is mandatory; the Intel display tail at the end is optional and is
 * only formatted when display_gen is not zero.
 */
struct gpu_info {
    bool present; /* a display controller was seen on PCI */
    bool known;   /* vendor probe identified the device */
    k_u16_t vendor;
    k_u16_t device;
    k_u8_t revision;
    k_u8_t bus;
    k_u8_t slot;
    k_u8_t function;
    k_u64_t mmio_base;         /* physical base of register MMIO BAR, 0 if unknown */
    k_u32_t mmio_size;         /* size of register MMIO BAR, 0 if unknown */
    k_u64_t vram_bytes;        /* VRAM/aperture size in bytes, 0 when unknown */
    k_u64_t vram_mc_bytes;     /* VRAM size from MC registers, 0 if unknown */
    k_u32_t mc_config_memsize; /* raw CONFIG_MEMSIZE (MiB units in low bits) */
    k_u32_t mc_arb_ramcfg;
    k_u32_t mc_shared_chmap;
    k_u32_t mc_vm_mx_l1_tlb_cntl;
    k_u32_t mc_seq_misc0;
    k_u32_t gb_addr_config;
    k_u8_t mc_num_channels;
    k_u8_t mc_chansize; /* 32 or 64 */
    k_u8_t gc_num_se;
    k_u8_t gc_num_sh_per_se;
    bool vram_read_ok;
    k_u32_t vram_sample;
    k_u64_t vram_probe_offset;
    char name[64]; /* human name / description, NUL-terminated */

    /* Optional Intel display detail: formatted when display_gen != 0. */
    k_u8_t display_gen;
    k_u8_t display_rel;
    char display_step[8]; /* "C0", "unknown", ... */
    k_u16_t panel_width;
    k_u16_t panel_height;

    /* The vendor probe outcome, for the dispatcher to map to kmod state. */
    enum gpu_probe_result probe_result;
};

struct gpu_probe {
    k_u16_t vendor;
    enum gpu_probe_result (*probe)(const struct port_x86_64_pci_device *pci, struct gpu_info *out);
};

/* Probe PCI display controllers and register the "gpu" kmod. Safe to call
 * after paging is up; it does not write display control registers. */
void port_x86_64_gpu_init(void);

/* The last probe result. present is false when no known GPU answered. */
const struct gpu_info *port_x86_64_gpu_info(void);

/* Format the boot/report line into buf. Returns buf. */
const char *port_x86_64_gpu_format(char *buf, k_size_t size);

/* Register the `gpu` shell command. */
int port_x86_64_gpu_register(void);

/* AMD vendor probe; returns GPU_PROBE_NONE/OK/UNKNOWN_DEV. */
enum gpu_probe_result port_x86_64_gpu_amd_probe(const struct port_x86_64_pci_device *pci,
                                                struct gpu_info *out);

/* Intel vendor probe is optional and may be linked in by another patch.
 * Declared weak so this tree builds without it. */
enum gpu_probe_result gpu_intel_probe(const struct port_x86_64_pci_device *pci,
                                      struct gpu_info *out) __attribute__((weak));

/* Optional Intel subcommands for the shared `gpu` command. */
int gpu_intel_command(int argc, char **argv) __attribute__((weak));

/* Vendor probe table: order matters, first matching vendor wins. Intel probe
 * is weak - NULL when not linked, which means "vendor known, device unknown". */
static const struct gpu_probe PORT_X86_64_GPU_PROBES[] = {
    {0x1002u, port_x86_64_gpu_amd_probe},
    {0x8086u, gpu_intel_probe},
};

#define PORT_X86_64_GPU_PROBE_COUNT                                                                \
    (sizeof(PORT_X86_64_GPU_PROBES) / sizeof(PORT_X86_64_GPU_PROBES[0]))

#endif /* MICRO_OS_PORT_X86_64_GPU_H */
