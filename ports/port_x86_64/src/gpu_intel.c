/*
 * Micro OS — the Intel GPU probe and read-only framebuffer inspection.
 *
 * Facts from drivers/gpu/drm/i915/i915_reg.h,
 * drivers/gpu/drm/i915/gt/intel_ggtt.c,
 * drivers/gpu/drm/i915/display/skl_universal_plane.c,
 * drivers/gpu/drm/i915/display/skl_universal_plane_regs.h and
 * drivers/gpu/drm/i915/display/intel_cursor_regs.h (GPL-2.0, Linux v6.12);
 * code written here for Micro OS (MIT).
 *
 * The probe never writes a display MMIO register. PCI memory decode is the only
 * control write, and even that is skipped until PMCSR says the card is in D0.
 */
#include "gpu_intel.h"

#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kshell/kshell.h"
#include "port_x86_64.h"
#include "port_x86_64_pci.h"

#include <stdio.h>
#include <string.h>

#define GPU_INTEL_MMIO_WINDOW (16u * 1024u * 1024u)
#define GPU_INTEL_GTT_ALIAS_OFFSET (8u * 1024u * 1024u)
#define GPU_INTEL_GTT_APERTURE_SIZE (256u * 1024u * 1024u)
#define GPU_INTEL_GTT_APERTURE_ENTRIES (GPU_INTEL_GTT_APERTURE_SIZE / GPU_INTEL_GTT_PAGE_SIZE)

/* The laptop first, then nearby Kaby Lake-R/Whiskey Lake identities. */
static const struct gpu_intel_id g_gpu_intel_ids[] = {
    {0x8086u, 0x5917u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u, GPU_INTEL_MMIO_WINDOW},
    {0x8086u, 0x5916u, "Intel HD 620", 9u, 0u, 3u, 4u, 5u, GPU_INTEL_MMIO_WINDOW},
    {0x8086u, 0x3EA0u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u, GPU_INTEL_MMIO_WINDOW},
    {0x8086u, 0x3EA9u, "Intel UHD 620", 9u, 0u, 3u, 4u, 5u, GPU_INTEL_MMIO_WINDOW},
    {0x8086u, 0x87C0u, "Intel UHD 617", 9u, 0u, 3u, 4u, 5u, GPU_INTEL_MMIO_WINDOW},
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

#define GPU_INTEL_MMIO_BYTES GPU_INTEL_MMIO_WINDOW
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

#define GPU_INTEL_PLANE_CTL_BASE 0x70180u
#define GPU_INTEL_PLANE_STRIDE_BASE 0x70188u
#define GPU_INTEL_PLANE_SIZE_BASE 0x70190u
#define GPU_INTEL_PLANE_SURF_BASE 0x7019Cu
#define GPU_INTEL_PIPE_REGISTER_STEP 0x1000u
#define GPU_INTEL_PLANE_REGISTER_STEP 0x100u
#define GPU_INTEL_CURSOR_CTL_BASE 0x70080u
#define GPU_INTEL_CURSOR_SURF_BASE 0x70084u
#define GPU_INTEL_CURSOR_GUARD_PAGES 64u
#define GPU_INTEL_APERTURE_PAGE 0x200000u

#define GPU_INTEL_PW1_STATE (1u << 28)
#define GPU_INTEL_DDI_ENABLE (1u << 31)
#define GPU_INTEL_TRANS_ENABLE (1u << 31)
#define GPU_INTEL_ACTIVE_MASK 0xFFFFu
#define GPU_INTEL_EDP_INPUT_MASK (7u << 12)
#define GPU_INTEL_PANEL_PIPE_UNKNOWN 0xFFu
#define GPU_INTEL_DIMENSION_MAX 16384u

static volatile k_u32_t *s_mmio;
static volatile k_u64_t *s_gtt_entries;
static k_u64_t s_aperture_base;

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

enum gpu_intel_find_result {
    GPU_INTEL_NOT_FOUND,
    GPU_INTEL_KNOWN,
    GPU_INTEL_UNKNOWN,
};

/* Separate an absent Intel card from an Intel card whose DID is not in the table. */
static enum gpu_intel_find_result gpu_intel_find(struct port_x86_64_pci_device *card,
                                                 const struct gpu_intel_id **identity)
{
    k_size_t i;

    for (i = 0; i < K_ARRAY_SIZE(g_gpu_intel_ids); i++) {
        if (port_x86_64_pci_find(g_gpu_intel_ids[i].vendor, g_gpu_intel_ids[i].device, card)) {
            *identity = &g_gpu_intel_ids[i];
            return GPU_INTEL_KNOWN;
        }
    }

    if (port_x86_64_pci_find_class(0x03u, 0x00u, 0u, card) &&
        port_x86_64_pci_read16(card, PCI_VENDOR) == PCI_INTEL_VENDOR) {
        return GPU_INTEL_UNKNOWN;
    }

    return GPU_INTEL_NOT_FOUND;
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
        s_gtt_entries = (volatile k_u64_t *)(uintptr_t)(PORT_X86_64_GPU_INTEL_VIRT + offset +
                                                        GPU_INTEL_GTT_ALIAS_OFFSET);
    }

    *base = window;
    return true;
}

static void gpu_intel_window(const struct gpu_info *info, char *text, k_size_t text_size)
{
    k_u32_t high = (k_u32_t)(info->mmio_base >> 32);
    k_u32_t low = (k_u32_t)(info->mmio_base & 0xFFFFFFFFu);
    unsigned mib = (unsigned)(info->mmio_size / (1024u * 1024u));

    if (high == 0u) {
        (void)snprintf(text, text_size, "0x%x+%uM", (unsigned)low, mib);
    } else {
        (void)snprintf(text, text_size, "0x%x%08x+%uM", (unsigned)high, (unsigned)low, mib);
    }
}

static void gpu_intel_address(k_u64_t address, char *text, k_size_t text_size)
{
    k_u32_t high = (k_u32_t)(address >> 32);
    k_u32_t low = (k_u32_t)(address & 0xFFFFFFFFu);

    if (high == 0u) {
        (void)snprintf(text, text_size, "%x", (unsigned)low);
    } else {
        (void)snprintf(text, text_size, "%x%08x", (unsigned)high, (unsigned)low);
    }
}

static bool gpu_intel_aperture(const struct port_x86_64_pci_device *card, struct gpu_info *info)
{
    k_u32_t bar = port_x86_64_pci_read32(card, PCI_BAR2);
    k_u32_t high = 0u;
    k_u64_t base;

    if ((bar & PCI_BAR_IO) != 0u || (bar & ~0x0Fu) == 0u) {
        return false;
    }
    if (((bar >> 1) & 3u) == 2u) {
        high = port_x86_64_pci_read32(card, PCI_BAR3);
    }

    base = ((k_u64_t)high << 32) | (k_u64_t)(bar & ~0x0Fu);
    if (base == 0u || (bar & ~0x0Fu) == 0xFFFFFFF0u) {
        return false;
    }

    info->gtt_aperture_base = base;
    info->gtt_aperture_size = GPU_INTEL_GTT_APERTURE_SIZE;
    s_aperture_base = base;
    return true;
}

static k_u32_t gpu_intel_pipe_register(k_u8_t pipe, k_u32_t base)
{
    return base + (k_u32_t)pipe * GPU_INTEL_PIPE_REGISTER_STEP;
}

static bool gpu_intel_read_framebuffer(struct gpu_info *info)
{
    k_u32_t control_offset;
    k_u32_t stride_offset;
    k_u32_t size_offset;
    k_u32_t surface_offset;
    k_u32_t plane_size;
    k_u32_t width;
    k_u32_t height;
    enum gpu_intel_ggtt_result rc;
    char physical[24];

    if (info->panel_pipe == GPU_INTEL_PANEL_PIPE_UNKNOWN || s_gtt_entries == NULL) {
        return false;
    }

    control_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_CTL_BASE);
    stride_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_STRIDE_BASE);
    size_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SIZE_BASE);
    surface_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURF_BASE);

    info->plane_control = gpu_intel_mmio_read(control_offset);
    info->plane_stride = gpu_intel_mmio_read(stride_offset);
    info->plane_surface = gpu_intel_mmio_read(surface_offset);
    plane_size = gpu_intel_mmio_read(size_offset);
    if (info->plane_control == 0xFFFFFFFFu || info->plane_stride == 0xFFFFFFFFu ||
        info->plane_surface == 0xFFFFFFFFu || plane_size == 0xFFFFFFFFu) {
        return false;
    }

    width = (plane_size & 0xFFFFu) + 1u;
    height = (plane_size >> 16) + 1u;
    if (width != info->panel_width || height != info->panel_height) {
        return false;
    }

    rc = gpu_intel_ggtt_decode_fb(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES,
                                  info->plane_surface, info->plane_stride, info->plane_control,
                                  width, height, &info->framebuffer);
    if (rc != GPU_INTEL_GGTT_OK) {
        kinfo("gpu: ggtt framebuffer is not readable (rc=%d)", (int)rc);
        return false;
    }

    info->framebuffer_known = true;
    gpu_intel_address(info->framebuffer.phys, physical, sizeof(physical));
    kinfo("gpu: ggtt framebuffer at 0x%s, %ux%u, pitch %u, %u bytes%s", physical,
          (unsigned)info->framebuffer.width, (unsigned)info->framebuffer.height,
          (unsigned)info->framebuffer.pitch, (unsigned)info->framebuffer.bytes,
          info->framebuffer.contiguous ? "" : ", fragmented");
    return true;
}

static bool gpu_intel_add_range(struct gpu_intel_gtt_range *ranges, k_size_t *count,
                                k_size_t capacity, k_u32_t start, k_u32_t pages)
{
    if (*count >= capacity || (k_u64_t)start + (k_u64_t)pages > GPU_INTEL_GTT_APERTURE_ENTRIES) {
        return false;
    }
    ranges[*count].start = start;
    ranges[*count].count = pages;
    (*count)++;
    return true;
}

static bool gpu_intel_collect_exclusions(const struct gpu_info *info,
                                         struct gpu_intel_gtt_range *ranges, k_size_t capacity,
                                         k_size_t *out_count)
{
    k_size_t count = 0u;
    k_u8_t pipe;
    k_u8_t plane;

    if (!info->framebuffer_known ||
        !gpu_intel_add_range(ranges, &count, capacity, info->framebuffer.pte_start,
                             info->framebuffer.pte_count)) {
        return false;
    }

    for (pipe = 0u; pipe < 3u; pipe++) {
        for (plane = 0u; plane < 3u; plane++) {
            k_u32_t base = gpu_intel_pipe_register(pipe, GPU_INTEL_PLANE_CTL_BASE) +
                           (k_u32_t)plane * GPU_INTEL_PLANE_REGISTER_STEP;
            k_u32_t control = gpu_intel_mmio_read(base);
            struct gpu_intel_fb surface;

            if (control == 0xFFFFFFFFu) {
                return false;
            }
            if ((control & GPU_INTEL_PLANE_ENABLE) != 0u) {
                k_u32_t stride = gpu_intel_mmio_read(base + 8u);
                k_u32_t size = gpu_intel_mmio_read(base + 16u);
                k_u32_t address = gpu_intel_mmio_read(base + 28u);
                enum gpu_intel_ggtt_result rc;

                if (stride == 0xFFFFFFFFu || size == 0xFFFFFFFFu || address == 0xFFFFFFFFu) {
                    return false;
                }
                rc = gpu_intel_ggtt_decode_fb(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES,
                                              address, stride, control, (size & 0xFFFFu) + 1u,
                                              (size >> 16) + 1u, &surface);
                if (rc != GPU_INTEL_GGTT_OK ||
                    !gpu_intel_add_range(ranges, &count, capacity, surface.pte_start,
                                         surface.pte_count)) {
                    return false;
                }
            }
        }

        {
            k_u32_t control =
                gpu_intel_mmio_read(gpu_intel_pipe_register(pipe, GPU_INTEL_CURSOR_CTL_BASE));
            k_u32_t address;

            if (control == 0xFFFFFFFFu) {
                return false;
            }
            if ((control & GPU_INTEL_PLANE_ENABLE) != 0u) {
                address =
                    gpu_intel_mmio_read(gpu_intel_pipe_register(pipe, GPU_INTEL_CURSOR_SURF_BASE));
                if (address == 0xFFFFFFFFu ||
                    !gpu_intel_add_range(ranges, &count, capacity,
                                         (address & GPU_INTEL_PLANE_SURFACE_MASK) >> 12,
                                         GPU_INTEL_CURSOR_GUARD_PAGES)) {
                    return false;
                }
            }
        }
    }

    *out_count = count;
    return true;
}

static volatile k_u8_t *gpu_intel_map_aperture(k_u64_t offset, k_size_t length)
{
    k_u64_t window;
    k_u64_t within;
    k_size_t map_length;

    if (offset > GPU_INTEL_GTT_APERTURE_SIZE ||
        (k_u64_t)length > (k_u64_t)GPU_INTEL_GTT_APERTURE_SIZE - offset) {
        return NULL;
    }

    window = offset & ~((k_u64_t)GPU_INTEL_APERTURE_PAGE - 1u);
    within = offset - window;
    map_length = (k_size_t)within + length;
    if (port_x86_64_paging_map_framebuffer(PORT_X86_64_GPU_INTEL_APERTURE_VIRT,
                                           s_aperture_base + window, map_length) == 0u) {
        return NULL;
    }

    return (volatile k_u8_t *)(uintptr_t)(PORT_X86_64_GPU_INTEL_APERTURE_VIRT + within);
}

static int gpu_intel_ggtt_test(const struct gpu_info *info)
{
    struct gpu_intel_gtt_range excluded[16];
    struct gpu_intel_gtt_run run;
    k_size_t excluded_count = 0u;
    void *allocation = NULL;
    k_u8_t *page = NULL;
    k_u64_t physical = 0u;
    volatile k_u32_t *aperture = NULL;
    k_u64_t neighbor_before;
    k_u64_t neighbor_after;
    k_u64_t original;
    k_u32_t pattern = 0x47545431u;
    bool installed = false;
    bool ok = false;
    k_size_t i;
    enum gpu_intel_ggtt_result rc;
    char physical_text[24];

    if (s_gtt_entries == NULL ||
        !gpu_intel_collect_exclusions(info, excluded, K_ARRAY_SIZE(excluded), &excluded_count)) {
        kshell_printf("gpu: live display surfaces cannot be excluded safely\n");
        return -1;
    }

    allocation = kmm_alloc(2u * GPU_INTEL_GTT_PAGE_SIZE, KMM_PRIORITY_KERNEL);
    if (allocation == NULL) {
        kshell_printf("gpu: no RAM page for the GGTT test\n");
        return -1;
    }
    page = (k_u8_t *)(((uintptr_t)allocation + GPU_INTEL_GTT_PAGE_SIZE - 1u) &
                      ~(uintptr_t)(GPU_INTEL_GTT_PAGE_SIZE - 1u));
    physical = port_x86_64_bus_phys(page);
    if (physical == 0u || (physical & (GPU_INTEL_GTT_PAGE_SIZE - 1u)) != 0u) {
        kshell_printf("gpu: the test page has no usable physical address\n");
        goto out;
    }

    rc = gpu_intel_ggtt_find_free(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, 1u, excluded,
                                  excluded_count, &run);
    if (rc != GPU_INTEL_GGTT_OK) {
        kshell_printf("gpu: no guarded free GGTT slot (rc=%d)\n", (int)rc);
        goto out;
    }

    original = s_gtt_entries[run.start];
    neighbor_before = s_gtt_entries[run.start - 1u];
    neighbor_after = s_gtt_entries[run.start + 1u];
    rc = gpu_intel_ggtt_borrow(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &run, physical,
                               &original);
    if (rc != GPU_INTEL_GGTT_OK) {
        kshell_printf("gpu: GGTT slot %u did not install (rc=%d)\n", (unsigned)run.start, (int)rc);
        goto out;
    }
    installed = true;

    aperture = (volatile k_u32_t *)gpu_intel_map_aperture(
        (k_u64_t)run.start * GPU_INTEL_GTT_PAGE_SIZE, GPU_INTEL_GTT_PAGE_SIZE);
    if (aperture == NULL) {
        kshell_printf("gpu: BAR2 cannot map GGTT slot %u\n", (unsigned)run.start);
        goto out;
    }

    for (i = 0u; i < GPU_INTEL_GTT_PAGE_SIZE / sizeof(k_u32_t); i++) {
        aperture[i] = pattern + (k_u32_t)i * 0x01010101u;
    }
    for (i = 0u; i < GPU_INTEL_GTT_PAGE_SIZE / sizeof(k_u32_t); i++) {
        k_u32_t expected = pattern + (k_u32_t)i * 0x01010101u;

        if (aperture[i] != expected || ((volatile k_u32_t *)page)[i] != expected) {
            kshell_printf("gpu: aperture mismatch at word %u\n", (unsigned)i);
            goto out;
        }
    }
    if (s_gtt_entries[run.start - 1u] != neighbor_before ||
        s_gtt_entries[run.start + 1u] != neighbor_after) {
        kshell_printf("gpu: a neighboring GGTT entry changed\n");
        goto out;
    }

    gpu_intel_address(physical, physical_text, sizeof(physical_text));
    kshell_printf("gpu: ggtt slot %u, aperture 0x%x, physical 0x%s, pattern 0x%08x: PASS\n",
                  (unsigned)run.start, (unsigned)((k_u64_t)run.start * GPU_INTEL_GTT_PAGE_SIZE),
                  physical_text, (unsigned)pattern);
    ok = true;

out:
    if (installed) {
        if (gpu_intel_ggtt_restore(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &run, original) !=
            GPU_INTEL_GGTT_OK) {
            kerror("gpu: GGTT slot %u could not be restored", (unsigned)run.start);
            ok = false;
        }
    }
    if (allocation != NULL) {
        kmm_free(allocation);
    }
    return ok ? 0 : -1;
}

static void gpu_intel_panel(const struct gpu_info *info)
{
    char window[40];

    gpu_intel_window(info, window, sizeof(window));
    if (info->panel_width != 0u && info->panel_height != 0u) {
        kinfo("gpu: %s (%04x:%04x rev %02x) at %02x:%02x.%x, "
              "mmio %s, display %u.%02u stepping %s, panel %ux%u",
              info->name, (unsigned)info->vendor, (unsigned)info->device, (unsigned)info->rev,
              (unsigned)info->bus, (unsigned)info->slot, (unsigned)info->func, window,
              (unsigned)info->display_gen, (unsigned)info->display_rel, info->display_step,
              (unsigned)info->panel_width, (unsigned)info->panel_height);
    } else {
        kinfo("gpu: %s (%04x:%04x rev %02x) at %02x:%02x.%x, "
              "mmio %s, display %u.%02u stepping %s, panel unknown",
              info->name, (unsigned)info->vendor, (unsigned)info->device, (unsigned)info->rev,
              (unsigned)info->bus, (unsigned)info->slot, (unsigned)info->func, window,
              (unsigned)info->display_gen, (unsigned)info->display_rel, info->display_step);
    }
}

int gpu_intel_probe(struct gpu_info *info)
{
    struct port_x86_64_pci_device card;
    const struct gpu_intel_id *identity = NULL;
    enum gpu_intel_find_result found;
    k_u8_t power_state;
    k_u16_t command;
    k_u16_t class_code;
    k_u32_t combined_wells;
    k_u32_t edp_conf;
    k_u32_t htotal;
    k_u32_t vtotal;
    k_u32_t width;
    k_u32_t height;
    bool aperture_ok;

    if (info == NULL) {
        return -1;
    }
    *info = (struct gpu_info){0};
    info->vram_bytes = GPU_INFO_UNKNOWN_SIZE;
    info->state = KMOD_SKIP;
    info->panel_pipe = GPU_INTEL_PANEL_PIPE_UNKNOWN;

    found = gpu_intel_find(&card, &identity);
    if (found == GPU_INTEL_NOT_FOUND) {
        kinfo("gpu: no Intel display found (KMOD_SKIP)");
        return 1;
    }

    info->vendor = port_x86_64_pci_read16(&card, PCI_VENDOR);
    info->device = port_x86_64_pci_read16(&card, PCI_DEVICE);
    info->rev = (k_u8_t)(port_x86_64_pci_read32(&card, PCI_REVISION) & 0xFFu);
    info->subsystem_vendor = port_x86_64_pci_read16(&card, PCI_SUBSYSTEM_VENDOR);
    info->subsystem_device = port_x86_64_pci_read16(&card, PCI_SUBSYSTEM_DEVICE);
    info->bus = card.bus;
    info->slot = card.slot;
    info->func = card.function;
    info->state = KMOD_UNKNOWN;

    if (found == GPU_INTEL_UNKNOWN) {
        info->name = "Intel unknown";
        kinfo("gpu: Intel %04x:%04x rev %02x at %02x:%02x.%x is not in the probe table "
              "(KMOD_UNKNOWN)",
              (unsigned)info->vendor, (unsigned)info->device, (unsigned)info->rev,
              (unsigned)info->bus, (unsigned)info->slot, (unsigned)info->func);
        return 1;
    }

    info->name = identity->name;
    info->mmio_size = identity->mmio_size;
    info->display_gen = identity->display_gen;
    info->display_rel = identity->display_rel;
    info->display_step = gpu_intel_display_step(info->device, info->rev);
    info->pipes = identity->pipes;
    info->transcoders = identity->transcoders;
    info->ports = identity->ports;

    class_code = port_x86_64_pci_read16(&card, PCI_CLASS);
    if (info->vendor != PCI_INTEL_VENDOR || info->device != identity->device ||
        card.function != 0u || (k_u8_t)(class_code >> 8) != 0x03u ||
        (k_u8_t)(class_code & 0xFFu) != 0x00u) {
        info->state = KMOD_FAIL;
        kerror("gpu: Intel PCI identity/class mismatch (KMOD_FAIL)");
        return -1;
    }

    if (!gpu_intel_power_state(&card, &power_state)) {
        info->state = KMOD_DEGRADED;
        kinfo("gpu: Intel power state is not readable; MMIO not touched (KMOD_DEGRADED)");
        return 1;
    }
    if (power_state != PCI_PM_D0) {
        info->state = KMOD_DEGRADED;
        kinfo("gpu: Intel is in D%u; MMIO not touched (KMOD_DEGRADED)", (unsigned)power_state);
        return 1;
    }

    command = port_x86_64_pci_read16(&card, PCI_COMMAND);
    if ((command & PCI_COMMAND_MEMORY) == 0u) {
        port_x86_64_pci_enable(&card, PCI_COMMAND_MEMORY);
    }

    if (!gpu_intel_map(&card, &info->mmio_base)) {
        info->state = KMOD_FAIL;
        kerror("gpu: Intel BAR0 is not a mappable memory window (KMOD_FAIL)");
        return -1;
    }

    aperture_ok = gpu_intel_aperture(&card, info);
    if (!aperture_ok) {
        info->state = KMOD_DEGRADED;
        kinfo("gpu: Intel BAR2 is not a mappable graphics aperture (KMOD_DEGRADED)");
        return 1;
    }

    info->fuse_status = gpu_intel_mmio_read(GPU_INTEL_SKL_FUSE_STATUS);
    info->power_well_bios = gpu_intel_mmio_read(GPU_INTEL_HSW_PWR_WELL_CTL1);
    info->power_well_driver = gpu_intel_mmio_read(GPU_INTEL_HSW_PWR_WELL_CTL2);
    if (info->fuse_status == 0xFFFFFFFFu || info->power_well_bios == 0xFFFFFFFFu ||
        info->power_well_driver == 0xFFFFFFFFu) {
        info->state = KMOD_FAIL;
        kerror("gpu: Intel MMIO did not answer (KMOD_FAIL)");
        return -1;
    }
    info->mmio_responded = true;

    combined_wells = info->power_well_bios | info->power_well_driver;
    info->display_ready = (combined_wells & GPU_INTEL_PW1_STATE) != 0u;
    if (!info->display_ready) {
        info->state = KMOD_DEGRADED;
        gpu_intel_panel(info);
        return 1;
    }

    info->de_port_isr = gpu_intel_mmio_read(GPU_INTEL_GEN8_DE_PORT_ISR);
    info->de_misc_isr = gpu_intel_mmio_read(GPU_INTEL_GEN8_DE_MISC_ISR);
    if (info->de_port_isr == 0xFFFFFFFFu || info->de_misc_isr == 0xFFFFFFFFu) {
        info->state = KMOD_FAIL;
        kerror("gpu: Intel display MMIO did not answer (KMOD_FAIL)");
        return -1;
    }

    info->edp_ctl = gpu_intel_mmio_read(GPU_INTEL_TRANS_DDI_FUNC_CTL_EDP);
    if (info->edp_ctl == 0xFFFFFFFFu) {
        info->state = KMOD_FAIL;
        kerror("gpu: Intel eDP state did not answer (KMOD_FAIL)");
        return -1;
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

    if ((info->panel_width != 0u || info->panel_height != 0u) &&
        !gpu_intel_read_framebuffer(info)) {
        info->state = KMOD_DEGRADED;
        gpu_intel_panel(info);
        return 1;
    }

    info->state = KMOD_OK;
    gpu_intel_panel(info);
    return 0;
}

static struct gpu_info g_gpu_info;
static int g_gpu_probe_rc = -1;

static int cmd_gpu(int argc, char **argv)
{
    char physical[24];

    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        kshell_printf("gpu: probe rc=%d, state=%d\n", g_gpu_probe_rc, (int)g_gpu_info.state);
        if (g_gpu_info.framebuffer_known) {
            gpu_intel_address(g_gpu_info.framebuffer.phys, physical, sizeof(physical));
            kshell_printf(
                "gpu: ggtt surface 0x%x, physical 0x%s, %ux%u, pitch %u, %u bytes\n",
                (unsigned)g_gpu_info.framebuffer.surface, physical,
                (unsigned)g_gpu_info.framebuffer.width, (unsigned)g_gpu_info.framebuffer.height,
                (unsigned)g_gpu_info.framebuffer.pitch, (unsigned)g_gpu_info.framebuffer.bytes);
        }
        return g_gpu_probe_rc == 0 ? KSHELL_OK : KSHELL_ERROR;
    }

    if (argc == 2 && strcmp(argv[1], "ggtt-test") == 0) {
        if (g_gpu_probe_rc != 0 || !g_gpu_info.framebuffer_known) {
            kshell_printf("gpu: a readable Intel framebuffer is required\n");
            return KSHELL_ERROR;
        }
        return gpu_intel_ggtt_test(&g_gpu_info) == 0 ? KSHELL_OK : KSHELL_ERROR;
    }

    kshell_printf("usage: gpu [info|ggtt-test]\n");
    return KSHELL_USAGE;
}

int gpu_intel_register(void)
{
    static const struct kshell_help help = {
        .summary = "inspect the Intel display and its global page table",
        .usage = "gpu [info|ggtt-test]",
        .detail = "Without a subcommand, or with info, prints the read-only probe result\n"
                  "and the firmware-left framebuffer mapping. ggtt-test temporarily maps\n"
                  "one RAM page into a guarded free GGTT slot, writes and reads it through\n"
                  "BAR2, checks both neighbors, and restores the original PTE.\n",
    };

    g_gpu_probe_rc = gpu_intel_probe(&g_gpu_info);
    return kshell_register_help("gpu", &help, cmd_gpu);
}
