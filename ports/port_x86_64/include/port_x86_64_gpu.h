/*
 * Micro OS — the x86-64 port: a display controller found on the PCI bus.
 *
 * The first step of a GPU driver is not drawing: it is to find the card, read
 * the identity it exposes without firmware, and say what answered. This header
 * is the small surface the boot and the shell share: the probe runs once, the
 * report is the same line the log carries, and no display control register is
 * written.
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

/* The result of one vendor probe: enough for the boot log and the `gpu`
 * command. All fields are read-only observations; no display state is changed. */
struct port_x86_64_gpu_info {
    bool found;
    k_u16_t vendor;
    k_u16_t device;
    k_u8_t revision;
    k_u8_t bus;
    k_u8_t slot;
    k_u8_t function;
    k_u64_t mmio_base;  /* physical base of the register MMIO BAR */
    k_u32_t mmio_size;  /* size of the register MMIO BAR */
    k_u64_t vram_bytes; /* VRAM aperture/size in bytes, 0 when unknown */
    char name[48];      /* human name, NUL-terminated */
};

/* Probe PCI display controllers and register the "gpu" kmod. Safe to call
 * after paging is up; it does not write display control registers. */
void port_x86_64_gpu_init(void);

/* The last probe result. found is false when no known GPU answered. */
const struct port_x86_64_gpu_info *port_x86_64_gpu_info(void);

/* Format the boot/report line into buf. Returns buf. */
const char *port_x86_64_gpu_format(char *buf, k_size_t size);

/* Register the `gpu` shell command. */
int port_x86_64_gpu_register(void);

/* AMD vendor probe; returns true when it claimed the device. */
bool port_x86_64_gpu_amd_probe(const struct port_x86_64_pci_device *pci,
                               struct port_x86_64_gpu_info *out);

#endif /* MICRO_OS_PORT_X86_64_GPU_H */
