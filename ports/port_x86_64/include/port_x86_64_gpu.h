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
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_H
#define MICRO_OS_PORT_X86_64_GPU_H

#include "kernel/types.h"

#include "port_x86_64_pci.h"

/* The MMIO window of the GPU registers in the top 2 GiB, after the e1000e
 * window. One 2 MiB page is enough for the 256 KiB register BAR of Polaris. */
#define PORT_X86_64_GPU_VIRT (PORT_X86_64_KERNEL_BASE + 0x3A000000ull)

enum gpu_probe_result {
    GPU_PROBE_NONE = 0,        /* not this vendor's responsibility */
    GPU_PROBE_OK = 1,          /* card identified */
    GPU_PROBE_UNKNOWN_DEV = 2, /* vendor recognized, device unknown */
};

/* The result of one vendor probe: enough for the boot log and the `gpu`
 * command. All fields are read-only observations; no display state is changed.
 *
 * This struct is the stable handshake surface for vendor probes. Do not change
 * it silently; Intel/x86 GPU work will build against it.
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
    k_u64_t mmio_base;  /* physical base of register MMIO BAR, 0 if unknown */
    k_u32_t mmio_size;  /* size of register MMIO BAR, 0 if unknown */
    k_u64_t vram_bytes; /* VRAM/aperture size in bytes, 0 when unknown */
    char name[64];      /* human name / description, NUL-terminated */
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

/* Vendor probe table: order matters, first matching vendor wins. Intel probe
 * is weak - NULL when not linked, which means "vendor known, device unknown". */
static const struct gpu_probe PORT_X86_64_GPU_PROBES[] = {
    {0x1002u, port_x86_64_gpu_amd_probe},
    {0x8086u, gpu_intel_probe},
};

#define PORT_X86_64_GPU_PROBE_COUNT                                                                \
    (sizeof(PORT_X86_64_GPU_PROBES) / sizeof(PORT_X86_64_GPU_PROBES[0]))

#endif /* MICRO_OS_PORT_X86_64_GPU_H */
