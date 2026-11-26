/*
 * Micro OS — the x86-64 port: AMD Polaris display controller probe.
 *
 * This is a read-only "hello" probe: it identifies the card from PCI config,
 * enables memory decode, records BAR layout, and optionally maps the register
 * MMIO BAR. It does not write display control registers, does not load
 * firmware, and does not start rings.
 *
 * Provenance:
 * - Implementation: original work for Micro OS.
 * - Hardware facts observed on a live system, 26 Nov 2026, card
 *   1002:67df rev ef (Sapphire Nitro+ RX 570/580/590, Ellesmere/Polaris10):
 *   BAR0 64-bit prefetchable 8 GiB VRAM aperture, BAR2 64-bit prefetchable 2 MiB,
 *   BAR4 I/O 256 bytes, BAR5 32-bit non-prefetchable 256 KiB register MMIO at
 *   0xfce00000. Observed via lspci/sysfs/journalctl; no Linux source copied.
 * - The PCI ID list is the minimal Ellesmere/Polaris10 set relevant to this
 *   probe; IDs and names correspond to public PCI ID assignments (1002:67df
 *   confirmed on hardware). No amdgpu source code was copied.
 */
#include "port_x86_64_gpu.h"
#include "port_x86_64_gpu_ids.h"

#include <string.h>

#include "kernel/log.h"
#include "port_x86_64.h"

static k_u64_t bar_base(const struct port_x86_64_pci_device *pci, k_u8_t offset)
{
    k_u32_t low = port_x86_64_pci_read32(pci, offset);
    k_u64_t base = (k_u64_t)(low & ~0xFu);

    if ((low & PCI_BAR_IO) != 0u) {
        return (k_u64_t)(low & ~0x3u);
    }

    /* 64-bit memory BAR: upper half in the next register. */
    if (((low >> 1) & 0x3u) == 0x2u) {
        k_u32_t high = port_x86_64_pci_read32(pci, (k_u8_t)(offset + 4u));
        base |= ((k_u64_t)high << 32);
    }

    return base;
}

static bool bar_is_mem64(const struct port_x86_64_pci_device *pci, k_u8_t offset)
{
    k_u32_t low = port_x86_64_pci_read32(pci, offset);

    return (low & PCI_BAR_IO) == 0u && (((low >> 1) & 0x3u) == 0x2u);
}

/* 64-bit BAR size probe: writes all-ones to low and high dwords, reads back
 * the mask, restores, and returns the size. This is standard PCI config space
 * probing, not a display engine register write. */
static k_u64_t bar_size64(const struct port_x86_64_pci_device *pci, k_u8_t offset)
{
    k_u32_t low_old = port_x86_64_pci_read32(pci, offset);
    k_u32_t high_old = port_x86_64_pci_read32(pci, (k_u8_t)(offset + 4u));
    k_u32_t low_mask;
    k_u32_t high_mask;
    k_u64_t mask;

    port_x86_64_pci_write32(pci, offset, 0xFFFFFFFFu);
    port_x86_64_pci_write32(pci, (k_u8_t)(offset + 4u), 0xFFFFFFFFu);
    low_mask = port_x86_64_pci_read32(pci, offset);
    high_mask = port_x86_64_pci_read32(pci, (k_u8_t)(offset + 4u));
    port_x86_64_pci_write32(pci, offset, low_old);
    port_x86_64_pci_write32(pci, (k_u8_t)(offset + 4u), high_old);

    low_mask &= ~0xFu;
    mask = ((k_u64_t)high_mask << 32) | (k_u64_t)low_mask;
    if (mask == 0u) {
        return 0;
    }

    return (~mask) + 1u;
}

bool port_x86_64_gpu_amd_probe(const struct port_x86_64_pci_device *pci,
                               struct port_x86_64_gpu_info *out)
{
    k_u16_t vendor;
    k_u16_t device;
    k_u16_t command;
    k_u8_t revision;
    k_u32_t bar5_low;
    k_u64_t bar0_base;
    k_u64_t bar5_base;
    k_u32_t bar0_size;
    k_u32_t bar5_size;
    k_u64_t out_vram = 0;
    const char *name;

    if (pci == NULL || out == NULL) {
        return false;
    }

    vendor = port_x86_64_pci_read16(pci, PCI_VENDOR);
    device = port_x86_64_pci_read16(pci, PCI_DEVICE);
    revision = (k_u8_t)(port_x86_64_pci_read16(pci, PCI_REVISION) & 0xFFu);

    name = port_x86_64_gpu_amd_lookup(vendor, device);
    if (name == NULL) {
        return false;
    }

    /* Enable memory decode so BAR MMIO can be read/mapped. This is PCI config,
     * not a display engine register write. Bus mastering is not needed for
     * read-only identification. */
    command = port_x86_64_pci_read16(pci, PCI_COMMAND);
    if ((command & PCI_COMMAND_MEMORY) == 0u) {
        port_x86_64_pci_enable(pci, PCI_COMMAND_MEMORY);
    }

    bar0_base = bar_base(pci, PCI_BAR0);
    if (bar_is_mem64(pci, PCI_BAR0)) {
        out_vram = bar_size64(pci, PCI_BAR0);
        bar0_size = 0;
    } else {
        bar0_size = port_x86_64_pci_bar_size(pci, PCI_BAR0);
        out_vram = (k_u64_t)bar0_size;
    }

    bar5_low = port_x86_64_pci_read32(pci, 0x24u); /* BAR5 */
    (void)bar5_low;
    bar5_base = bar_base(pci, 0x24u);
    bar5_size = port_x86_64_pci_bar_size(pci, 0x24u);

    memset(out, 0, sizeof(*out));
    out->found = true;
    out->vendor = vendor;
    out->device = device;
    out->revision = revision;
    out->bus = pci->bus;
    out->slot = pci->slot;
    out->function = pci->function;
    out->mmio_base = bar5_base;
    out->mmio_size = bar5_size;

    /* BAR0 is the VRAM aperture on Polaris10 discrete cards. Use its size as
     * the reported VRAM/aperture size; the value is a hardware fact from the
     * PCI BAR size probe, not a firmware query. */
    if (bar0_base != 0u) {
        out->vram_bytes = out_vram;
    }

    {
        k_size_t n = 0;

        while (name[n] != '\0' && n + 1u < sizeof(out->name)) {
            out->name[n] = name[n];
            n++;
        }
        out->name[n] = '\0';
    }

    /* Map register MMIO BAR (BAR5) as uncached MMIO. We do not read display
     * engine registers; the mapping is established so a later, validated read
     * can use it. Mapping failure is not fatal for PCI identification. */
    if (out->mmio_base != 0u && out->mmio_size != 0u) {
        k_u32_t pages = port_x86_64_paging_map_mmio(PORT_X86_64_GPU_VIRT, out->mmio_base,
                                                    (k_size_t)out->mmio_size);
        if (pages == 0u) {
            kwarn("gpu: BAR5 MMIO 0x%x size 0x%x could not be mapped",
                  (unsigned)(k_u32_t)out->mmio_base, (unsigned)out->mmio_size);
        } else {
            kinfo("gpu: BAR5 MMIO mapped at virt 0x%x (phys 0x%x, %u KiB)",
                  (unsigned)(k_u32_t)PORT_X86_64_GPU_VIRT, (unsigned)(k_u32_t)out->mmio_base,
                  (unsigned)(out->mmio_size / 1024u));
        }
    }

    return true;
}
