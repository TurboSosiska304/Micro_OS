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

#include "kernel/log.h"
#include "port_x86_64.h"
#include "port_x86_64_pci.h"

/* The laptop first, then nearby Kaby Lake-R/Whiskey Lake identities. */
static const struct gpu_intel_id g_gpu_intel_ids[] = {
    {0x8086u, 0x5917u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u},
    {0x8086u, 0x5916u, "Intel HD 620", 9u, 0u, 3u, 4u, 5u},
    {0x8086u, 0x3EA0u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u},
    {0x8086u, 0x3EA9u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u},
    {0x8086u, 0x87C0u, "Intel UHD 617", 9u, 0u, 3u, 4u, 5u},
};

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

/*
 * Kaby Lake's PCI revision does not equal either IP stepping. The public
 * i915 revision table maps rev 07 to display C0 (and GT G0); other KBL
 * revisions differ. Unknown families/revisions stay explicitly unknown.
 */
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

/* Read-only state and identity registers justified in the reconnaissance. */
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
#define GPU_INTEL_PANEL_PIPE_UNKNOWN 0xFFu
#define GPU_INTEL_DIMENSION_MAX 16384u

static volatile k_u32_t *s_mmio;

static k_u32_t gpu_intel_mmio_read(k_u32_t offset)
{
    return s_mmio[offset / sizeof(k_u32_t)];
}

/* A bounded, read-only capability walk; a missing power capability is unsafe. */
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

static bool gpu_intel_find(struct port_x86_64_pci_device *card,
                           const struct gpu_intel_id **identity)
{
    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(g_gpu_intel_ids); i++) {
        if (port_x86_64_pci_find(g_gpu_intel_ids[i].vendor, g_gpu_intel_ids[i].device, card)) {
            *identity = &g_gpu_intel_ids[i];
            return true;
        }
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
        if (port_x86_64_paging_map_mmio(PORT_X86_64_GPU_INTEL_VIRT, window - offset,
                                        (k_size_t)(offset + GPU_INTEL_MMIO_BYTES)) == 0u) {
            return false;
        }
        s_mmio = (volatile k_u32_t *)(uintptr_t)(PORT_X86_64_GPU_INTEL_VIRT + offset);
    }

    *base = window;
    return true;
}

static void gpu_intel_panel(const struct gpu_intel_info *info)
{
    if (info->panel_width != 0u && info->panel_height != 0u) {
        kinfo("gpu: %s (%04x:%04x rev %02x) at %02x:%02x.%x, "
              "mmio 0x%08x%08x, display %u.%02u stepping %s, panel %ux%u",
              info->name, (unsigned)info->vendor, (unsigned)info->device, (unsigned)info->revision,
              (unsigned)info->bus, (unsigned)info->slot, (unsigned)info->function,
              (unsigned)(info->mmio_base >> 32), (unsigned)(info->mmio_base & 0xFFFFFFFFu),
              (unsigned)info->display_gen, (unsigned)info->display_rel, info->display_step,
              (unsigned)info->panel_width, (unsigned)info->panel_height);
    } else {
        kinfo("gpu: %s (%04x:%04x rev %02x) at %02x:%02x.%x, "
              "mmio 0x%08x%08x, display %u.%02u stepping %s, panel unknown",
              info->name, (unsigned)info->vendor, (unsigned)info->device, (unsigned)info->revision,
              (unsigned)info->bus, (unsigned)info->slot, (unsigned)info->function,
              (unsigned)(info->mmio_base >> 32), (unsigned)(info->mmio_base & 0xFFFFFFFFu),
              (unsigned)info->display_gen, (unsigned)info->display_rel, info->display_step);
    }
}

kmod_state_t gpu_intel_probe(struct gpu_intel_info *info)
{
    struct port_x86_64_pci_device card;
    const struct gpu_intel_id *identity = NULL;
    k_u8_t power_state;
    k_u16_t command;
    k_u16_t class_code;
    k_u32_t combined_wells;
    k_u32_t edp_conf;
    k_u32_t htotal;
    k_u32_t vtotal;
    k_u32_t width;
    k_u32_t height;

    if (info == NULL) {
        return KMOD_FAIL;
    }
    *info = (struct gpu_intel_info){0};
    info->panel_pipe = GPU_INTEL_PANEL_PIPE_UNKNOWN;

    if (!gpu_intel_find(&card, &identity)) {
        kinfo("gpu: no supported Intel display found (KMOD_SKIP)");
        return KMOD_SKIP;
    }

    info->name = identity->name;
    info->vendor = port_x86_64_pci_read16(&card, PCI_VENDOR);
    info->device = port_x86_64_pci_read16(&card, PCI_DEVICE);
    info->revision = (k_u8_t)(port_x86_64_pci_read32(&card, PCI_REVISION) & 0xFFu);
    info->subsystem_vendor = port_x86_64_pci_read16(&card, PCI_SUBSYSTEM_VENDOR);
    info->subsystem_device = port_x86_64_pci_read16(&card, PCI_SUBSYSTEM_DEVICE);
    info->bus = card.bus;
    info->slot = card.slot;
    info->function = card.function;
    info->display_gen = identity->display_gen;
    info->display_rel = identity->display_rel;
    info->display_step = gpu_intel_display_step(info->device, info->revision);
    info->pipes = identity->pipes;
    info->transcoders = identity->transcoders;
    info->ports = identity->ports;

    class_code = port_x86_64_pci_read16(&card, PCI_CLASS);
    if (info->vendor != PCI_INTEL_VENDOR || info->device != identity->device ||
        card.function != 0u || (k_u8_t)(class_code >> 8) != 0x03u ||
        (k_u8_t)(class_code & 0xFFu) != 0x00u) {
        kerror("gpu: Intel PCI identity/class mismatch (KMOD_FAIL)");
        return KMOD_FAIL;
    }

    if (!gpu_intel_power_state(&card, &power_state)) {
        kinfo("gpu: Intel power state is not readable; MMIO not touched (KMOD_SKIP)");
        return KMOD_SKIP;
    }
    if (power_state != PCI_PM_D0) {
        kinfo("gpu: Intel is in D%u; MMIO not touched (KMOD_SKIP)", (unsigned)power_state);
        return KMOD_SKIP;
    }

    command = port_x86_64_pci_read16(&card, PCI_COMMAND);
    if ((command & PCI_COMMAND_MEMORY) == 0u) {
        port_x86_64_pci_enable(&card, PCI_COMMAND_MEMORY);
    }

    if (!gpu_intel_map(&card, &info->mmio_base)) {
        kerror("gpu: Intel BAR0 is not a mappable memory window (KMOD_FAIL)");
        return KMOD_FAIL;
    }

    info->fuse_status = gpu_intel_mmio_read(GPU_INTEL_SKL_FUSE_STATUS);
    info->power_well_bios = gpu_intel_mmio_read(GPU_INTEL_HSW_PWR_WELL_CTL1);
    info->power_well_driver = gpu_intel_mmio_read(GPU_INTEL_HSW_PWR_WELL_CTL2);
    if (info->fuse_status == 0xFFFFFFFFu || info->power_well_bios == 0xFFFFFFFFu ||
        info->power_well_driver == 0xFFFFFFFFu) {
        kerror("gpu: Intel MMIO did not answer (KMOD_FAIL)");
        return KMOD_FAIL;
    }
    info->mmio_responded = true;

    combined_wells = info->power_well_bios | info->power_well_driver;
    info->display_ready = (combined_wells & GPU_INTEL_PW1_STATE) != 0u;
    if (!info->display_ready) {
        gpu_intel_panel(info);
        return KMOD_DEGRADED;
    }

    info->de_port_isr = gpu_intel_mmio_read(GPU_INTEL_GEN8_DE_PORT_ISR);
    info->de_misc_isr = gpu_intel_mmio_read(GPU_INTEL_GEN8_DE_MISC_ISR);
    if (info->de_port_isr == 0xFFFFFFFFu || info->de_misc_isr == 0xFFFFFFFFu) {
        kerror("gpu: Intel display MMIO did not answer (KMOD_FAIL)");
        return KMOD_FAIL;
    }

    info->edp_ctl = gpu_intel_mmio_read(GPU_INTEL_TRANS_DDI_FUNC_CTL_EDP);
    if (info->edp_ctl == 0xFFFFFFFFu) {
        kerror("gpu: Intel eDP state did not answer (KMOD_FAIL)");
        return KMOD_FAIL;
    }

    if ((info->edp_ctl & GPU_INTEL_DDI_ENABLE) != 0u) {
        edp_conf = gpu_intel_mmio_read(GPU_INTEL_TRANSCONF_EDP);
        htotal = gpu_intel_mmio_read(GPU_INTEL_TRANS_HTOTAL_EDP);
        vtotal = gpu_intel_mmio_read(GPU_INTEL_TRANS_VTOTAL_EDP);
        if (edp_conf != 0xFFFFFFFFu && htotal != 0xFFFFFFFFu && vtotal != 0xFFFFFFFFu &&
            (edp_conf & GPU_INTEL_TRANS_ENABLE) != 0u) {
            width = (htotal & GPU_INTEL_ACTIVE_MASK) + 1u;
            height = (vtotal & GPU_INTEL_ACTIVE_MASK) + 1u;
            if (width <= GPU_INTEL_DIMENSION_MAX && height <= GPU_INTEL_DIMENSION_MAX) {
                info->panel_width = (k_u16_t)width;
                info->panel_height = (k_u16_t)height;
                switch (info->edp_ctl & GPU_INTEL_EDP_INPUT_MASK) {
                case 0u:
                case 4u << 12:
                    info->panel_pipe = 0u;
                    break;
                case 5u << 12:
                    info->panel_pipe = 1u;
                    break;
                case 6u << 12:
                    info->panel_pipe = 2u;
                    break;
                default:
                    break;
                }
            }
        }
    }

    gpu_intel_panel(info);
    return KMOD_OK;
}
