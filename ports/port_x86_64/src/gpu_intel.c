/*
 * Micro OS — the read-only Intel GPU probe of the x86-64 port.
 *
 * Provenance: PCI identities and Kaby Lake display/register facts were learned
 * from Intel's public hardware documentation and the GPL-licensed Linux i915
 * driver. No i915 source text, macros, control flow or data-structure
 * expression was copied; this implementation and its layout are original.
 *
 * The probe never writes a display MMIO register. PCI memory decode is the only
 * control write, and even that is skipped until PMCSR says the card is in D0.
 */
#include "gpu_intel.h"

#include <stdio.h>
#include <string.h>

#include "kernel/log.h"
#include "port_x86_64.h"

#define PCI_INTEL_VENDOR 0x8086u
#define PCI_STATUS 0x06u
#define PCI_SUBSYSTEM_VENDOR 0x2Cu
#define PCI_SUBSYSTEM_DEVICE 0x2Eu
#define PCI_CAP_POINTER 0x34u
#define PCI_CAP_ID_POWER 0x01u
#define PCI_CAP_LIST 0x0010u
#define PCI_PM_STATE_MASK 0x0003u
#define PCI_PM_D0 0u

#define GPU_INTEL_MMIO_BYTES 0x80000u
#define GPU_INTEL_MMIO_PAGE 0x200000u

/* Read-only state and identity registers. */
#define GPU_INTEL_SKL_FUSE_STATUS 0x42000u
#define GPU_INTEL_GEN8_DE_PORT_ISR 0x44440u
#define GPU_INTEL_GEN8_DE_MISC_ISR 0x44460u
#define GPU_INTEL_HSW_PWR_WELL_CTL1 0x45400u
#define GPU_INTEL_HSW_PWR_WELL_CTL2 0x45404u
#define GPU_INTEL_TRANS_DDI_FUNC_CTL_EDP 0x6F400u
#define GPU_INTEL_TRANS_HTOTAL_EDP 0x6F000u
#define GPU_INTEL_TRANS_VTOTAL_EDP 0x6F00Cu
#define GPU_INTEL_TRANSCONF_EDP 0x7F008u

#define GPU_INTEL_PW1_STATE (1u << 28)
#define GPU_INTEL_DDI_ENABLE (1u << 31)
#define GPU_INTEL_TRANS_ENABLE (1u << 31)
#define GPU_INTEL_ACTIVE_MASK 0xFFFFu
#define GPU_INTEL_EDP_INPUT_MASK (7u << 12)
#define GPU_INTEL_DIMENSION_MAX 16384u

/* The laptop first, then nearby Kaby Lake-R/Whiskey Lake identities. */
static const struct gpu_intel_id g_gpu_intel_ids[] = {
    {0x8086u, 0x5917u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u, 16u * 1024u * 1024u},
    {0x8086u, 0x5916u, "Intel HD 620", 9u, 0u, 3u, 4u, 5u, 16u * 1024u * 1024u},
    {0x8086u, 0x3EA0u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u, 16u * 1024u * 1024u},
    {0x8086u, 0x3EA9u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u, 16u * 1024u * 1024u},
    {0x8086u, 0x87C0u, "Intel UHD 617", 9u, 0u, 3u, 4u, 5u, 16u * 1024u * 1024u},
};

static volatile k_u32_t *s_mmio;

const struct gpu_intel_id *gpu_intel_id_lookup(k_u16_t vendor, k_u16_t device)
{
    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(g_gpu_intel_ids); i++) {
        if (g_gpu_intel_ids[i].vendor == vendor && g_gpu_intel_ids[i].device == device) {
            return &g_gpu_intel_ids[i];
        }
    }

    return NULL;
}

static const char *const g_gpu_intel_kbl_display_steps[] = {
    NULL, "B0", "B0", "B0", "C0", "B1", "B1", "C0",
};

static const char *gpu_intel_display_step(k_u16_t device, k_u8_t revision)
{
    if ((device == 0x5917u || device == 0x5916u || device == 0x87C0u) &&
        (k_size_t)revision < K_ARRAY_SIZE(g_gpu_intel_kbl_display_steps)) {
        const char *step = g_gpu_intel_kbl_display_steps[revision];

        return step != NULL ? step : "unknown";
    }

    return "unknown";
}

static k_u32_t gpu_intel_mmio_read(k_u32_t offset)
{
    return s_mmio[offset / sizeof(k_u32_t)];
}

static bool gpu_intel_power_state(const struct port_x86_64_pci_device *card, k_u8_t *state)
{
    k_u16_t status = port_x86_64_pci_read16(card, PCI_STATUS);
    k_u8_t pointer;
    k_u8_t guard;

    if ((status & PCI_CAP_LIST) == 0u) {
        return false;
    }

    pointer = (k_u8_t)(port_x86_64_pci_read32(card, PCI_CAP_POINTER) & 0xFCu);
    for (guard = 0; guard < 32u && pointer >= 0x40u; guard++) {
        k_u32_t head = port_x86_64_pci_read32(card, pointer);
        k_u8_t next = (k_u8_t)((head >> 8) & 0xFCu);

        if ((k_u8_t)(head & 0xFFu) == PCI_CAP_ID_POWER) {
            if (pointer > 0xF8u) {
                return false;
            }
            *state =
                (k_u8_t)(port_x86_64_pci_read16(card, (k_u8_t)(pointer + 4u)) & PCI_PM_STATE_MASK);
            return true;
        }

        if (next <= pointer) {
            break;
        }
        pointer = next;
    }

    return false;
}

static bool gpu_intel_map(const struct port_x86_64_pci_device *card, k_u64_t *base)
{
    k_u32_t bar = port_x86_64_pci_read32(card, PCI_BAR0);
    k_u32_t high = 0u;
    k_u64_t window;
    k_u64_t offset;

    if ((bar & PCI_BAR_IO) != 0u || (bar & ~0x0Fu) == 0u) {
        return false;
    }

    if (((bar >> 1) & 3u) == 2u) {
        high = port_x86_64_pci_read32(card, PCI_BAR1);
    }

    window = ((k_u64_t)high << 32) | (k_u64_t)(bar & ~0x0Fu);
    if (window == 0u || (bar & ~0x0Fu) == 0xFFFFFFF0u) {
        return false;
    }

    if (s_mmio == NULL) {
        offset = window & (GPU_INTEL_MMIO_PAGE - 1u);
        if (port_x86_64_paging_map_mmio(PORT_X86_64_GPU_VIRT, window - offset,
                                        (k_size_t)(offset + GPU_INTEL_MMIO_BYTES)) == 0u) {
            return false;
        }
        s_mmio = (volatile k_u32_t *)(uintptr_t)(PORT_X86_64_GPU_VIRT + offset);
    }

    *base = window;
    return true;
}

static void gpu_intel_fill_common(struct gpu_info *out, const struct port_x86_64_pci_device *pci,
                                  k_u16_t vendor, k_u16_t device, k_u8_t revision,
                                  const struct gpu_intel_id *id)
{
    out->present = true;
    out->vendor = vendor;
    out->device = device;
    out->revision = revision;
    out->bus = pci->bus;
    out->slot = pci->slot;
    out->function = pci->function;
    out->vram_bytes = 0u;
    if (id != NULL) {
        snprintf(out->name, sizeof(out->name), "%s", id->name);
        out->mmio_size = (k_u32_t)id->mmio_size;
        out->display_gen = id->display_gen;
        out->display_rel = id->display_rel;
        snprintf(out->display_step, sizeof(out->display_step), "%s",
                 gpu_intel_display_step(device, revision));
    } else {
        snprintf(out->name, sizeof(out->name), "Intel display controller");
    }
}

enum gpu_probe_result gpu_intel_probe(const struct port_x86_64_pci_device *pci,
                                      struct gpu_info *out)
{
    k_u16_t vendor;
    k_u16_t device;
    k_u8_t revision;
    const struct gpu_intel_id *id;
    k_u8_t power_state;
    k_u16_t command;
    k_u32_t combined_wells;
    k_u32_t edp_conf;
    k_u32_t htotal;
    k_u32_t vtotal;
    k_u32_t width;
    k_u32_t height;

    if (pci == NULL || out == NULL) {
        return GPU_PROBE_NONE;
    }

    vendor = port_x86_64_pci_read16(pci, PCI_VENDOR);
    device = port_x86_64_pci_read16(pci, PCI_DEVICE);
    revision = (k_u8_t)(port_x86_64_pci_read16(pci, PCI_REVISION) & 0xFFu);

    if (vendor != PCI_INTEL_VENDOR) {
        return GPU_PROBE_NONE;
    }

    id = gpu_intel_id_lookup(vendor, device);
    if (id == NULL) {
        gpu_intel_fill_common(out, pci, vendor, device, revision, NULL);
        out->known = false;
        out->probe_result = GPU_PROBE_UNKNOWN_DEV;
        return GPU_PROBE_UNKNOWN_DEV;
    }

    gpu_intel_fill_common(out, pci, vendor, device, revision, id);
    out->known = true;

    if (!gpu_intel_power_state(pci, &power_state)) {
        kinfo("gpu: Intel %04x:%04x power state is not readable; MMIO not touched",
              (unsigned)vendor, (unsigned)device);
        out->probe_result = GPU_PROBE_DEGRADED;
        return GPU_PROBE_DEGRADED;
    }
    if (power_state != PCI_PM_D0) {
        kinfo("gpu: Intel %04x:%04x is in D%u; MMIO not touched (KMOD_DEGRADED)", (unsigned)vendor,
              (unsigned)device, (unsigned)power_state);
        out->probe_result = GPU_PROBE_DEGRADED;
        return GPU_PROBE_DEGRADED;
    }

    command = port_x86_64_pci_read16(pci, PCI_COMMAND);
    if ((command & PCI_COMMAND_MEMORY) == 0u) {
        port_x86_64_pci_enable(pci, PCI_COMMAND_MEMORY);
    }

    if (!gpu_intel_map(pci, &out->mmio_base)) {
        kerror("gpu: Intel BAR0 is not a mappable memory window (KMOD_FAIL)");
        out->probe_result = GPU_PROBE_FAIL;
        return GPU_PROBE_FAIL;
    }

    {
        k_u32_t fuse = gpu_intel_mmio_read(GPU_INTEL_SKL_FUSE_STATUS);
        k_u32_t pw1 = gpu_intel_mmio_read(GPU_INTEL_HSW_PWR_WELL_CTL1);
        k_u32_t pw2 = gpu_intel_mmio_read(GPU_INTEL_HSW_PWR_WELL_CTL2);

        if (fuse == 0xFFFFFFFFu || pw1 == 0xFFFFFFFFu || pw2 == 0xFFFFFFFFu) {
            kerror("gpu: Intel MMIO did not answer (KMOD_FAIL)");
            out->probe_result = GPU_PROBE_FAIL;
            return GPU_PROBE_FAIL;
        }

        combined_wells = pw1 | pw2;
        if ((combined_wells & GPU_INTEL_PW1_STATE) == 0u) {
            out->probe_result = GPU_PROBE_DEGRADED;
            return GPU_PROBE_DEGRADED;
        }

        {
            k_u32_t de_port = gpu_intel_mmio_read(GPU_INTEL_GEN8_DE_PORT_ISR);
            k_u32_t de_misc = gpu_intel_mmio_read(GPU_INTEL_GEN8_DE_MISC_ISR);
            k_u32_t edp_ctl = gpu_intel_mmio_read(GPU_INTEL_TRANS_DDI_FUNC_CTL_EDP);

            if (de_port == 0xFFFFFFFFu || de_misc == 0xFFFFFFFFu || edp_ctl == 0xFFFFFFFFu) {
                kerror("gpu: Intel display MMIO did not answer (KMOD_FAIL)");
                out->probe_result = GPU_PROBE_FAIL;
                return GPU_PROBE_FAIL;
            }

            if ((edp_ctl & GPU_INTEL_DDI_ENABLE) != 0u) {
                edp_conf = gpu_intel_mmio_read(GPU_INTEL_TRANSCONF_EDP);
                htotal = gpu_intel_mmio_read(GPU_INTEL_TRANS_HTOTAL_EDP);
                vtotal = gpu_intel_mmio_read(GPU_INTEL_TRANS_VTOTAL_EDP);
                if (edp_conf != 0xFFFFFFFFu && htotal != 0xFFFFFFFFu && vtotal != 0xFFFFFFFFu &&
                    (edp_conf & GPU_INTEL_TRANS_ENABLE) != 0u) {
                    width = (htotal & GPU_INTEL_ACTIVE_MASK) + 1u;
                    height = (vtotal & GPU_INTEL_ACTIVE_MASK) + 1u;
                    if (width <= GPU_INTEL_DIMENSION_MAX && height <= GPU_INTEL_DIMENSION_MAX) {
                        out->panel_width = (k_u16_t)width;
                        out->panel_height = (k_u16_t)height;
                    }
                }
            }
        }
    }

    out->probe_result = GPU_PROBE_OK;
    return GPU_PROBE_OK;
}
