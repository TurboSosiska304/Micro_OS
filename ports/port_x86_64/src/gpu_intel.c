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
 * The probe path never writes a display MMIO register. PCI memory decode is the
 * only control write there. Display-register writes live in the confirmed
 * `gpu flip --yes` experiment and in the automatic `present` backend, which is
 * registered only for the same measured laptop and a compatible linear plane.
 */
#include "gpu_intel.h"

#ifdef MICRO_OS_HOST_TEST
#include "test_x86_64_ticks.h"
#else
#define wifi_test_ticks port_x86_64_ticks
#endif

#include "kernel/blit.h"
#include "kernel/errno.h"
#include "kernel/fb.h"
#include "kernel/fb_ops.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/logfile.h"
#include "kshell/kshell.h"
#include "port_x86_64.h"
#include "port_x86_64_pci.h"
#include "port_x86_64_screen.h"

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
#define GPU_INTEL_PLANE_SURFLIVE_BASE 0x701ACu
#define GPU_INTEL_PIPE_FRAME_BASE 0x70040u
#define GPU_INTEL_PIPE_REGISTER_STEP 0x1000u
#define GPU_INTEL_PLANE_REGISTER_STEP 0x100u
#define GPU_INTEL_CURSOR_CTL_BASE 0x70080u
#define GPU_INTEL_CURSOR_SURF_BASE 0x70084u
#define GPU_INTEL_CURSOR_GUARD_PAGES 64u
#define GPU_INTEL_APERTURE_PAGE 0x200000u

#define GPU_INTEL_RENDER_DECOMPRESSION (1u << 15)
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

#define GPU_INTEL_FLIP_SURFACES 2u
#define GPU_INTEL_CACHE_LINE 64u

struct gpu_intel_flip_slot {
    bool mapped;
    k_u64_t base;
    k_u32_t width;
    k_u32_t height;
    k_u32_t pitch;
    struct gpu_intel_gtt_run run;
    k_u64_t *physical;
};

struct gpu_intel_flip_state {
    bool reserved;
    int active;
    struct gpu_intel_flip_slot slots[GPU_INTEL_FLIP_SURFACES];
};

static struct gpu_intel_flip_state s_flip;

static k_u32_t gpu_intel_mmio_read(k_u32_t offset)
{
    return s_mmio[offset / sizeof(k_u32_t)];
}

static void gpu_intel_mmio_write(k_u32_t offset, k_u32_t value)
{
    s_mmio[offset / sizeof(k_u32_t)] = value;
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

static bool gpu_intel_aperture(const struct port_x86_64_pci_device *card,
                               struct gpu_intel_info *info)
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
    info->common.vram_bytes = GPU_INTEL_GTT_APERTURE_SIZE;
    s_aperture_base = base;
    return true;
}

static k_u32_t gpu_intel_pipe_register(k_u8_t pipe, k_u32_t base)
{
    return base + (k_u32_t)pipe * GPU_INTEL_PIPE_REGISTER_STEP;
}

static bool gpu_intel_read_framebuffer(struct gpu_intel_info *info)
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

    {
        struct gpu_intel_plane_ctl ctl = gpu_intel_plane_ctl_decode(info->plane_control);

        kinfo("gpu: plane: %s, %s, %ux%u, stride 0x%x", gpu_intel_format_name(ctl.format),
              gpu_intel_tiling_name(ctl.tiling), (unsigned)width, (unsigned)height,
              (unsigned)info->plane_stride);
        kinfo("gpu: plane regs: CTL 0x%08x STRIDE 0x%08x SIZE 0x%08x SURF 0x%08x",
              (unsigned)info->plane_control, (unsigned)info->plane_stride, (unsigned)plane_size,
              (unsigned)info->plane_surface);
    }
    if (width != info->common.panel_width || height != info->common.panel_height) {
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

static bool gpu_intel_collect_exclusions(const struct gpu_intel_info *info,
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

static int gpu_intel_ggtt_test(const struct gpu_intel_info *info)
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
    bool keep_buffer = false;
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

    ok = true;

out:
    if (installed) {
        if (gpu_intel_ggtt_restore(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &run, original) !=
            GPU_INTEL_GGTT_OK) {
            kerror("gpu: GGTT slot %u could not be restored; keeping RAM alive",
                   (unsigned)run.start);
            ok = false;
            keep_buffer = true;
        }
    }
    if (allocation != NULL && !keep_buffer) {
        kmm_free(allocation);
    }
    if (ok) {
        gpu_intel_address(physical, physical_text, sizeof(physical_text));
        kshell_printf("gpu: ggtt slot %u, aperture 0x%x, physical 0x%s, pattern 0x%08x: PASS\n",
                      (unsigned)run.start, (unsigned)((k_u64_t)run.start * GPU_INTEL_GTT_PAGE_SIZE),
                      physical_text, (unsigned)pattern);
    }
    return ok ? 0 : -1;
}

static bool gpu_intel_flip_laptop(const struct gpu_intel_info *info)
{
    return info->common.vendor == 0x8086u && info->common.device == 0x5917u &&
           info->common.revision == 0x07u && info->subsystem_vendor == 0x17AAu &&
           info->subsystem_device == 0x225Cu;
}

static bool gpu_intel_wait_frame(const struct gpu_intel_info *info)
{
    /* Kaby Lake uses the G4X full frame counter, not the split PIPEFRAME pair.
     * The tick deadline is the fast path; the spin bound keeps a stalled or
     * missing tick source from turning a frame wait into an infinite loop. */
    k_u32_t offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PIPE_FRAME_BASE);
    k_u32_t before = gpu_intel_mmio_read(offset);
    k_u64_t deadline = wifi_test_ticks() + 100u;
    k_u32_t spins = 0u;

    do {
        if (gpu_intel_mmio_read(offset) != before) {
            return true;
        }
        spins++;
    } while (wifi_test_ticks() < deadline && spins < 4000000u);

    if (spins >= 4000000u || wifi_test_ticks() >= deadline) {
        kwarn("gpu: frame wait timed out (pipe %u, before 0x%x, now 0x%x, spins %u)",
              (unsigned)info->panel_pipe, (unsigned)before, (unsigned)gpu_intel_mmio_read(offset),
              (unsigned)spins);
    }
    return false;
}

static bool gpu_intel_plane_program(const struct gpu_intel_info *info, k_u32_t control,
                                    k_u32_t stride, k_u32_t surface)
{
    k_u32_t control_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_CTL_BASE);
    k_u32_t stride_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_STRIDE_BASE);
    k_u32_t surface_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURF_BASE);

    gpu_intel_mmio_write(control_offset, control);
    gpu_intel_mmio_write(stride_offset, stride);
    gpu_intel_mmio_write(surface_offset, surface);
    return gpu_intel_mmio_read(control_offset) == control &&
           gpu_intel_mmio_read(stride_offset) == stride &&
           gpu_intel_mmio_read(surface_offset) == surface;
}

/* The old surface is written first: rollback must not leave the plane armed on new RAM. */
static bool gpu_intel_plane_restore(const struct gpu_intel_info *info, k_u32_t control,
                                    k_u32_t stride, k_u32_t surface)
{
    k_u32_t control_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_CTL_BASE);
    k_u32_t stride_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_STRIDE_BASE);
    k_u32_t surface_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURF_BASE);

    gpu_intel_mmio_write(surface_offset, surface);
    gpu_intel_mmio_write(stride_offset, stride);
    gpu_intel_mmio_write(control_offset, control);
    return gpu_intel_mmio_read(surface_offset) == surface &&
           gpu_intel_mmio_read(stride_offset) == stride &&
           gpu_intel_mmio_read(control_offset) == control;
}

static bool gpu_intel_present_ready(const struct gpu_intel_info *info)
{
    return info->display_ready && info->framebuffer_known &&
           info->panel_pipe != GPU_INTEL_PANEL_PIPE_UNKNOWN && s_gtt_entries != NULL &&
           gpu_intel_flip_laptop(info) &&
           (info->plane_surface & ~GPU_INTEL_PLANE_SURFACE_MASK) == 0u &&
           (info->plane_control & GPU_INTEL_RENDER_DECOMPRESSION) == 0u &&
           (info->plane_control & 3u) == 0u && info->framebuffer.tiling == GPU_INTEL_TILE_LINEAR &&
           ((info->framebuffer.format == GPU_INTEL_FORMAT_RGB_565 && info->framebuffer.bpp == 2u) ||
            (info->framebuffer.format == GPU_INTEL_FORMAT_XRGB_8888 &&
             info->framebuffer.bpp == 4u));
}

static bool gpu_intel_present_compatible(const struct gpu_intel_info *info,
                                         const struct k_fb_surface *dst,
                                         const struct k_fb_surface *src)
{
    k_u8_t expected = info->framebuffer.format == GPU_INTEL_FORMAT_XRGB_8888 ? FB_FORMAT_XRGB8888
                                                                             : FB_FORMAT_RGB565;

    return info->common.probe_result == GPU_PROBE_OK && gpu_intel_present_ready(info) &&
           dst != NULL && src != NULL && dst->width == info->framebuffer.width &&
           dst->height == info->framebuffer.height && src->width == dst->width &&
           src->height == dst->height &&
           (k_u64_t)dst->pitch * (k_u64_t)info->framebuffer.bpp == info->framebuffer.pitch &&
           src->pitch == dst->pitch && dst->format == expected && src->format == expected;
}

/* Reserve two guarded runs before the first flip. Both surfaces stay mapped
 * while they alternate, so a frame never has to allocate or copy. */
static bool gpu_intel_present_reserve(const struct gpu_intel_info *info)
{
    struct gpu_intel_gtt_range excluded[16];
    k_size_t excluded_count = 0u;
    enum gpu_intel_ggtt_result rc;

    if (s_flip.reserved) {
        return true;
    }
    if (!gpu_intel_collect_exclusions(info, excluded, K_ARRAY_SIZE(excluded), &excluded_count)) {
        return false;
    }

    rc = gpu_intel_ggtt_find_free(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES,
                                  info->framebuffer.pte_count, excluded, excluded_count,
                                  &s_flip.slots[0].run);
    if (rc != GPU_INTEL_GGTT_OK ||
        !gpu_intel_add_range(excluded, &excluded_count, K_ARRAY_SIZE(excluded),
                             s_flip.slots[0].run.start, s_flip.slots[0].run.count)) {
        return false;
    }

    rc = gpu_intel_ggtt_find_free(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES,
                                  info->framebuffer.pte_count, excluded, excluded_count,
                                  &s_flip.slots[1].run);
    if (rc != GPU_INTEL_GGTT_OK) {
        return false;
    }

    s_flip.reserved = true;
    s_flip.active = -1;
    return true;
}

static int gpu_intel_present_map(const struct gpu_intel_info *info, const struct k_fb_surface *src)
{
    k_size_t index;
    k_size_t count = info->framebuffer.pte_count;
    int free_slot = -1;
    int slot_index;
    struct gpu_intel_flip_slot *slot;
    k_u64_t saved = 0u;
    k_size_t held = 0u;
    enum gpu_intel_ggtt_result rc;

    for (slot_index = 0; slot_index < (int)GPU_INTEL_FLIP_SURFACES; slot_index++) {
        slot = &s_flip.slots[slot_index];
        if (!slot->mapped) {
            if (free_slot < 0) {
                free_slot = slot_index;
            }
            continue;
        }
        if (slot->base == (k_u64_t)(uintptr_t)src->base) {
            if (slot->width != src->width || slot->height != src->height ||
                slot->pitch != src->pitch || port_x86_64_dma_phys(src->base) != slot->physical[0] ||
                port_x86_64_dma_phys((const k_u8_t *)src->base +
                                     (k_size_t)(count - 1u) * GPU_INTEL_GTT_PAGE_SIZE) !=
                    slot->physical[count - 1u]) {
                return KERR_NOTSUP;
            }
            return 0;
        }
    }

    if (free_slot < 0 || ((k_u64_t)(uintptr_t)src->base & (GPU_INTEL_GTT_PAGE_SIZE - 1u)) != 0u) {
        return KERR_NOTSUP;
    }
    slot = &s_flip.slots[free_slot];
    slot->physical = kmm_alloc(count * sizeof(*slot->physical), KMM_PRIORITY_KERNEL);
    if (slot->physical == NULL) {
        return KERR_NOMEM;
    }

    for (index = 0u; index < count; index++) {
        k_u64_t physical =
            port_x86_64_dma_phys((const k_u8_t *)src->base + index * GPU_INTEL_GTT_PAGE_SIZE);

        if (physical == 0u || (physical & (GPU_INTEL_GTT_PAGE_SIZE - 1u)) != 0u ||
            (physical & ~GPU_INTEL_GTT_ADDRESS_MASK) != 0u || !port_x86_64_frame_hold(physical)) {
            goto fail;
        }
        slot->physical[index] = physical;
        held++;
    }

    rc = gpu_intel_ggtt_install(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &slot->run,
                                slot->physical, &saved);
    if (rc != GPU_INTEL_GGTT_OK) {
        goto fail;
    }

    slot->mapped = true;
    slot->base = (k_u64_t)(uintptr_t)src->base;
    slot->width = src->width;
    slot->height = src->height;
    slot->pitch = src->pitch;
    return 0;

fail:
    for (index = 0u; index < held; index++) {
        port_x86_64_frame_release(slot->physical[index]);
    }
    kmm_free(slot->physical);
    slot->physical = NULL;
    return KERR_NOTSUP;
}

/* The display engine reads physical RAM, not the CPU cache of this port. Make
 * the visible cache lines write-back before the plane is allowed to fetch. */
static void gpu_intel_present_flush(const struct k_fb_surface *src)
{
    const k_u8_t *base = (const k_u8_t *)(const void *)src->base;
    k_size_t bytes =
        (k_size_t)src->pitch * (k_size_t)src->height * (k_size_t)fb_pixel_bytes(src->format);
    k_size_t offset;

    for (offset = 0u; offset < bytes; offset += GPU_INTEL_CACHE_LINE) {
        const void *line = base + offset;

        __asm__ volatile("clflush (%0)" : : "r"(line) : "memory");
    }
    __asm__ volatile("mfence" : : : "memory");
}

static k_u32_t gpu_intel_present_address(const struct gpu_intel_flip_slot *slot)
{
    return slot->run.start * GPU_INTEL_GTT_PAGE_SIZE;
}

static bool gpu_intel_present_is_live(const struct gpu_intel_info *info, k_u32_t surface)
{
    k_u32_t live_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURFLIVE_BASE);

    return (gpu_intel_mmio_read(live_offset) & GPU_INTEL_PLANE_SURFACE_MASK) ==
           (surface & GPU_INTEL_PLANE_SURFACE_MASK);
}

static bool gpu_intel_present_restore(const struct gpu_intel_info *info)
{
    k_logfile_flush();
    if (!gpu_intel_wait_frame(info) ||
        !gpu_intel_plane_restore(info, info->plane_control, info->plane_stride,
                                 info->plane_surface) ||
        !gpu_intel_wait_frame(info) || !gpu_intel_present_is_live(info, info->plane_surface)) {
        return false;
    }

    s_flip.active = -1;
    return true;
}

static int gpu_intel_present(void *context, const struct k_fb_surface *dst,
                             const struct k_fb_surface *src)
{
    const struct gpu_intel_info *info = (const struct gpu_intel_info *)context;
    k_u32_t control_offset;
    k_u32_t stride_offset;
    k_u32_t surface_offset;
    k_u32_t expected_surface;
    k_u32_t target_surface;
    bool display_touched = false;
    int slot_index;
    int rc;

    if (!gpu_intel_present_compatible(info, dst, src)) {
        kwarn("gpu: present incompatible (plane %ux%u fmt %u pitch %u, dst %ux%u fmt %u pitch %u, "
              "src %ux%u fmt %u pitch %u)",
              (unsigned)info->framebuffer.width, (unsigned)info->framebuffer.height,
              (unsigned)info->framebuffer.format, (unsigned)info->framebuffer.pitch,
              dst != NULL ? (unsigned)dst->width : 0u, dst != NULL ? (unsigned)dst->height : 0u,
              dst != NULL ? (unsigned)dst->format : 0u, dst != NULL ? (unsigned)dst->pitch : 0u,
              src != NULL ? (unsigned)src->width : 0u, src != NULL ? (unsigned)src->height : 0u,
              src != NULL ? (unsigned)src->format : 0u, src != NULL ? (unsigned)src->pitch : 0u);
        if (s_flip.active >= 0 && !gpu_intel_present_restore(info)) {
            kerror("gpu: present could not restore the original plane after a refusal");
        }
        return KERR_NOTSUP;
    }
    if (!gpu_intel_present_reserve(info)) {
        kwarn("gpu: present reserve failed (no GGTT slot for %u pages)",
              (unsigned)info->framebuffer.pte_count);
        if (s_flip.active >= 0 && !gpu_intel_present_restore(info)) {
            kerror("gpu: present could not restore the original plane after reserve failure");
        }
        return KERR_NOTSUP;
    }

    rc = gpu_intel_present_map(info, src);
    if (rc != 0) {
        if (s_flip.active >= 0 && !gpu_intel_present_restore(info)) {
            kerror("gpu: present could not restore the original plane after a mapping refusal");
        }
        return rc;
    }
    for (slot_index = 0; slot_index < (int)GPU_INTEL_FLIP_SURFACES; slot_index++) {
        if (s_flip.slots[slot_index].mapped &&
            s_flip.slots[slot_index].base == (k_u64_t)(uintptr_t)src->base) {
            break;
        }
    }
    if (slot_index == (int)GPU_INTEL_FLIP_SURFACES) {
        goto fail;
    }

    control_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_CTL_BASE);
    stride_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_STRIDE_BASE);
    surface_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURF_BASE);
    expected_surface = s_flip.active >= 0 ? gpu_intel_present_address(&s_flip.slots[s_flip.active])
                                          : info->plane_surface;
    if (gpu_intel_mmio_read(control_offset) != info->plane_control ||
        gpu_intel_mmio_read(stride_offset) != info->plane_stride ||
        gpu_intel_mmio_read(surface_offset) != expected_surface ||
        !gpu_intel_present_is_live(info, expected_surface)) {
        goto fail;
    }

    target_surface = gpu_intel_present_address(&s_flip.slots[slot_index]);
    if (s_flip.active == slot_index) {
        return 0;
    }

    gpu_intel_present_flush(src);
    display_touched = true;
    k_logfile_flush();
    if (!gpu_intel_plane_program(info, info->plane_control, info->plane_stride, target_surface) ||
        !gpu_intel_wait_frame(info) || !gpu_intel_present_is_live(info, target_surface)) {
        kwarn("gpu: present did not go live (target 0x%x, active %d)", (unsigned)target_surface,
              s_flip.active);
        goto fail;
    }

    s_flip.active = slot_index;
    return 0;

fail:
    /* Once a RAM surface has been live, even a failure before the next plane
     * write must restore the original surface: the software fallback copies
     * into that framebuffer, not into the last RAM front buffer. */
    if ((display_touched || s_flip.active >= 0) && !gpu_intel_present_restore(info)) {
        kerror("gpu: present rollback is not live; keeping both GGTT surfaces mapped");
    }
    return KERR_NOTSUP;
}

static int gpu_intel_present_release(void *context)
{
    const struct gpu_intel_info *info = (const struct gpu_intel_info *)context;
    bool ok = true;
    k_size_t index;
    int slot_index;

    if (!s_flip.reserved) {
        return 0;
    }
    if (!gpu_intel_present_restore(info)) {
        return -1;
    }

    for (slot_index = 0; slot_index < (int)GPU_INTEL_FLIP_SURFACES; slot_index++) {
        struct gpu_intel_flip_slot *slot = &s_flip.slots[slot_index];

        if (!slot->mapped) {
            continue;
        }
        if (gpu_intel_ggtt_restore(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &slot->run,
                                   slot->run.original) != GPU_INTEL_GGTT_OK) {
            ok = false;
            continue;
        }
        for (index = 0u; index < slot->run.count; index++) {
            port_x86_64_frame_release(slot->physical[index]);
        }
        kmm_free(slot->physical);
        memset(slot, 0, sizeof(*slot));
    }

    if (ok) {
        s_flip.reserved = false;
        s_flip.active = -1;
    }
    return ok ? 0 : -1;
}

static const struct k_fb_ops s_gpu_intel_present_ops = {
    .present = gpu_intel_present,
    .present_release = gpu_intel_present_release,
};

static struct {
    bool active;
    void *allocation;
    k_u8_t *page;
    k_u64_t physical;
    struct gpu_intel_gtt_run run;
    k_u64_t saved_gtt;
    k_u32_t old_control;
    k_u32_t old_stride;
    k_u32_t old_surface;
    k_u64_t old_fb_phys;
    k_u32_t old_fb_pte_start;
    k_u32_t old_fb_pte_count;
    k_u8_t old_fb_tiling;
    bool old_fb_contiguous;
} s_linear;

static void gpu_intel_detile_x(const k_u8_t *src, k_u8_t *dst, k_u32_t width, k_u32_t height,
                               k_u32_t pitch, k_u8_t bpp)
{
    k_u32_t tile_stride = pitch / 512u;
    k_u32_t y;

    for (y = 0u; y < height; y++) {
        k_u32_t x;

        for (x = 0u; x < width; x++) {
            k_u32_t byte_x = x * (k_u32_t)bpp;
            k_u32_t tile_x = byte_x / 512u;
            k_u32_t tile_y = y / 8u;
            k_u32_t in_x = byte_x % 512u;
            k_u32_t in_y = y % 8u;
            k_size_t src_off =
                ((k_size_t)tile_y * (k_size_t)tile_stride + (k_size_t)tile_x) * 4096u +
                (k_size_t)in_y * 512u + (k_size_t)in_x;
            k_size_t dst_off = (k_size_t)y * (k_size_t)pitch + (k_size_t)byte_x;

            memcpy(dst + dst_off, src + src_off, bpp);
        }
    }
}

static int gpu_intel_plane_linear(struct gpu_intel_info *info, bool confirmed);

static bool gpu_intel_plane_linear_restore(struct gpu_intel_info *info)
{
    k_u32_t live_offset;
    bool ok = true;

    if (!s_linear.active) {
        return true;
    }

    if (s_flip.reserved && gpu_intel_present_release(info) != 0) {
        kerror("gpu: display enable: could not release the present backend before rollback");
        ok = false;
    }

    k_logfile_flush();

    live_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURFLIVE_BASE);

    if (!gpu_intel_plane_restore(info, s_linear.old_control, s_linear.old_stride,
                                 s_linear.old_surface) ||
        !gpu_intel_wait_frame(info) || !gpu_intel_wait_frame(info) ||
        (gpu_intel_mmio_read(live_offset) & GPU_INTEL_PLANE_SURFACE_MASK) != s_linear.old_surface) {
        kerror("gpu: display enable: original plane is not live after rollback");
        return false;
    }

    if (gpu_intel_ggtt_restore(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &s_linear.run,
                               s_linear.saved_gtt) != GPU_INTEL_GGTT_OK) {
        kerror("gpu: display enable: could not restore GGTT after plane rollback");
        ok = false;
    }

    info->plane_control = s_linear.old_control;
    info->plane_stride = s_linear.old_stride;
    info->plane_surface = s_linear.old_surface;
    info->framebuffer.surface = s_linear.old_surface;
    info->framebuffer.phys = s_linear.old_fb_phys;
    info->framebuffer.pte_start = s_linear.old_fb_pte_start;
    info->framebuffer.pte_count = s_linear.old_fb_pte_count;
    info->framebuffer.tiling = s_linear.old_fb_tiling;
    info->framebuffer.contiguous = s_linear.old_fb_contiguous;

    if (s_linear.allocation != NULL) {
        kmm_free(s_linear.allocation);
    }

    memset(&s_linear, 0, sizeof(s_linear));
    k_fb_ops_register(NULL, NULL);
    port_x86_64_fb_set_format(info->framebuffer.format == GPU_INTEL_FORMAT_XRGB_8888
                                  ? FB_FORMAT_XRGB8888
                                  : FB_FORMAT_RGB565,
                              (k_u8_t)(info->framebuffer.bpp * 8u));

    if (ok) {
        kinfo("gpu: plane-linear: restored original plane 0x%08x", (unsigned)info->plane_surface);
    }

    return ok;
}

static int gpu_intel_display_enable(struct gpu_intel_info *info, bool confirmed)
{
    struct k_fb_surface screen;
    struct k_fb_surface src;
    void *allocation = NULL;
    k_u8_t *page = NULL;
    k_size_t bytes;
    k_u8_t pixel_bytes;
    int rc;

    if (info->common.probe_result != GPU_PROBE_OK || !info->display_ready ||
        !info->framebuffer_known || info->panel_pipe == GPU_INTEL_PANEL_PIPE_UNKNOWN ||
        s_gtt_entries == NULL) {
        kshell_printf("gpu: display enable needs an active, readable Intel display\n");
        return -1;
    }
    if (!gpu_intel_flip_laptop(info)) {
        kshell_printf("gpu: display enable is limited to the measured ThinkPad 17aa:225c\n");
        return -1;
    }

    if (!confirmed) {
        if (info->framebuffer.tiling == GPU_INTEL_TILE_LINEAR) {
            kshell_printf("gpu: display enable dry-run: plane is already linear %s, %ux%u\n",
                          gpu_intel_format_name(info->framebuffer.format),
                          (unsigned)info->framebuffer.width, (unsigned)info->framebuffer.height);
        } else {
            kshell_printf("gpu: display enable dry-run: would run plane-linear %s %s -> linear, "
                          "then verify k_blit_present returns device\n",
                          gpu_intel_format_name(info->framebuffer.format),
                          gpu_intel_tiling_name(info->framebuffer.tiling));
        }
        kshell_printf(
            "gpu: display enable dry-run: nothing written; use gpu display enable --yes\n");
        kshell_printf("gpu: owner checklist: if the laptop shows a black screen, reboot restores "
                      "the firmware plane\n");
        return 0;
    }

    if (info->framebuffer.tiling != GPU_INTEL_TILE_LINEAR) {
        rc = gpu_intel_plane_linear(info, true);
        if (rc != 0) {
            kshell_printf(
                "gpu: display enable: plane-linear failed; plane not changed to linear\n");
            return -1;
        }
    }

    if (!gpu_intel_present_ready(info)) {
        const char *reason = "present backend is not ready for this plane";

        if ((info->plane_control & GPU_INTEL_RENDER_DECOMPRESSION) != 0u) {
            reason = "plane has render decompression enabled";
        } else if ((info->plane_control & 3u) != 0u) {
            reason = "plane rotation is not identity";
        } else if (info->framebuffer.tiling != GPU_INTEL_TILE_LINEAR) {
            reason = "plane is not linear";
        } else if (info->framebuffer.format != GPU_INTEL_FORMAT_RGB_565 &&
                   info->framebuffer.format != GPU_INTEL_FORMAT_XRGB_8888) {
            reason = "plane format is not RGB565/XRGB8888";
        }
        kshell_printf("gpu: display enable: %s\n", reason);
        if (s_linear.active) {
            k_logfile_flush();
            if (!gpu_intel_plane_linear_restore(info)) {
                kerror("gpu: display enable: rollback failed; reboot restores the firmware plane");
                kshell_printf("gpu: display enable: rollback failed; reboot restores the firmware "
                              "plane\n");
            } else {
                kshell_printf("gpu: display enable: plane restored to original %s %s\n",
                              gpu_intel_format_name(info->framebuffer.format),
                              gpu_intel_tiling_name(info->framebuffer.tiling));
            }
        }
        return -1;
    }

    if (!port_x86_64_fb_surface(&screen)) {
        kshell_printf("gpu: display enable: no framebuffer surface to verify present\n");
        if (s_linear.active) {
            k_logfile_flush();
            (void)gpu_intel_plane_linear_restore(info);
        }
        return -1;
    }

    pixel_bytes = fb_pixel_bytes(screen.format);
    bytes = (k_size_t)screen.pitch * (k_size_t)screen.height * (k_size_t)pixel_bytes;
    allocation = kmm_alloc(bytes + GPU_INTEL_GTT_PAGE_SIZE - 1u, KMM_PRIORITY_KERNEL);
    if (allocation == NULL) {
        kshell_printf("gpu: display enable: no RAM for present verification buffer\n");
        if (s_linear.active) {
            k_logfile_flush();
            (void)gpu_intel_plane_linear_restore(info);
        }
        return -1;
    }

    page = (k_u8_t *)(((uintptr_t)allocation + GPU_INTEL_GTT_PAGE_SIZE - 1u) &
                      ~(uintptr_t)(GPU_INTEL_GTT_PAGE_SIZE - 1u));
    memcpy(page, (const void *)(uintptr_t)screen.base, bytes);

    src = screen;
    src.base = (k_u16_t *)(void *)page;

    rc = k_blit_present(&screen, &src);
    if (rc != 0) {
        const char *why =
            (rc == 1) ? "k_blit_present returned kernel copy" : "k_blit_present returned an error";

        kerror("gpu: display enable: present verification failed: %s (rc=%d)", why, rc);
        kshell_printf("gpu: display enable: %s (rc=%d); restoring plane\n", why, rc);
        k_logfile_flush();
        if (s_linear.active) {
            if (!gpu_intel_plane_linear_restore(info)) {
                kerror("gpu: display enable: rollback failed; reboot restores the firmware plane");
                kshell_printf("gpu: display enable: rollback failed; reboot restores the firmware "
                              "plane\n");
            } else {
                kshell_printf("gpu: display enable: plane restored\n");
            }
        } else {
            kshell_printf("gpu: display enable: plane was already linear; reboot restores the "
                          "firmware plane if the screen is wrong\n");
        }
        /* Keep allocation alive: the present backend may still hold GGTT mappings. */
        (void)allocation;
        return -1;
    }

    kshell_printf("gpu: display enable: present verified (device path, rc=0)\n");
    kshell_printf("gpu: owner checklist: if the laptop shows a black screen, reboot restores the "
                  "firmware plane\n");
    /* The verification buffer remains mapped as a present slot; keep it alive. */
    return 0;
}

static int gpu_intel_plane_linear(struct gpu_intel_info *info, bool confirmed)
{
    struct gpu_intel_gtt_range excluded[16];
    k_size_t excluded_count = 0u;
    struct gpu_intel_plane_ctl ctl;
    k_u32_t control_offset;
    k_u32_t stride_offset;
    k_u32_t surface_offset;
    k_u32_t live_offset;
    k_u32_t control;
    k_u32_t stride;
    k_u32_t surface;
    k_u32_t new_control;
    k_u32_t new_stride;
    k_u32_t new_surface;
    k_u32_t plane_size;
    k_u32_t width;
    k_u32_t height;
    k_u64_t bytes;
    void *allocation = NULL;
    k_u8_t *page = NULL;
    k_u64_t physical = 0u;
    volatile k_u8_t *source = NULL;
    bool run_installed = false;
    bool display_touched = false;
    bool ok = false;
    bool keep_buffer = false;
    enum gpu_intel_ggtt_result rc;

    if (info->common.probe_result != GPU_PROBE_OK || !info->display_ready ||
        !info->framebuffer_known || info->panel_pipe == GPU_INTEL_PANEL_PIPE_UNKNOWN ||
        s_gtt_entries == NULL) {
        kshell_printf("gpu: plane-linear needs an active, readable Intel display\n");
        return -1;
    }
    if (!gpu_intel_flip_laptop(info)) {
        kshell_printf("gpu: plane-linear is limited to the measured ThinkPad 17aa:225c\n");
        return -1;
    }
    if ((info->plane_surface & ~GPU_INTEL_PLANE_SURFACE_MASK) != 0u ||
        (info->plane_control & GPU_INTEL_RENDER_DECOMPRESSION) != 0u ||
        (info->plane_control & 3u) != 0u) {
        kshell_printf("gpu: plane-linear refuses surface flags, compression or rotation\n");
        return -1;
    }

    control_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_CTL_BASE);
    stride_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_STRIDE_BASE);
    surface_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURF_BASE);
    live_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURFLIVE_BASE);

    control = gpu_intel_mmio_read(control_offset);
    stride = gpu_intel_mmio_read(stride_offset);
    surface = gpu_intel_mmio_read(surface_offset);
    plane_size =
        gpu_intel_mmio_read(gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SIZE_BASE));
    if (control != info->plane_control || stride != info->plane_stride ||
        surface != info->plane_surface) {
        kshell_printf("gpu: the display changed after the probe; run gpu info again\n");
        return -1;
    }

    ctl = gpu_intel_plane_ctl_decode(control);
    width = (plane_size & 0xFFFFu) + 1u;
    height = (plane_size >> 16) + 1u;
    if (!ctl.valid || width != info->framebuffer.width || height != info->framebuffer.height) {
        kshell_printf("gpu: plane-linear cannot decode the current plane\n");
        return -1;
    }

    new_control = (control & ~GPU_INTEL_PLANE_TILE_MASK) | ((k_u32_t)GPU_INTEL_TILE_LINEAR << 10);
    new_stride = (info->framebuffer.pitch / 64u) & GPU_INTEL_PLANE_STRIDE_MASK;

    kshell_printf("gpu: plane-linear %s: %s %s -> %s linear, %ux%u, ctl 0x%08x -> 0x%08x, stride "
                  "0x%x -> 0x%x\n",
                  confirmed ? "plan" : "dry-run", gpu_intel_format_name(ctl.format),
                  gpu_intel_tiling_name(ctl.tiling), gpu_intel_format_name(ctl.format),
                  (unsigned)width, (unsigned)height, (unsigned)control, (unsigned)new_control,
                  (unsigned)stride, (unsigned)new_stride);

    if (ctl.tiling == GPU_INTEL_TILE_LINEAR) {
        kshell_printf("gpu: plane is already linear; no register write needed\n");
        return 0;
    }
    if (ctl.tiling != GPU_INTEL_TILE_X) {
        kshell_printf("gpu: plane-linear supports X-tiled sources only\n");
        return -1;
    }
    if ((info->framebuffer.pitch % 64u) != 0u ||
        (info->framebuffer.pitch / 64u) > GPU_INTEL_PLANE_STRIDE_MASK) {
        kshell_printf("gpu: plane-linear cannot express the linear stride\n");
        return -1;
    }

    if (!confirmed) {
        kshell_printf("gpu: plane-linear dry-run: nothing written; use gpu plane-linear --yes\n");
        return 0;
    }

    if (!gpu_intel_collect_exclusions(info, excluded, K_ARRAY_SIZE(excluded), &excluded_count)) {
        kshell_printf("gpu: plane-linear cannot exclude live display surfaces\n");
        return -1;
    }
    rc = gpu_intel_ggtt_find_free(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES,
                                  info->framebuffer.pte_count, excluded, excluded_count,
                                  &s_linear.run);
    if (rc != GPU_INTEL_GGTT_OK) {
        kshell_printf("gpu: no guarded GGTT run for %u pages (rc=%d)\n",
                      (unsigned)info->framebuffer.pte_count, (int)rc);
        return -1;
    }
    new_surface = s_linear.run.start * GPU_INTEL_GTT_PAGE_SIZE;

    bytes = (k_u64_t)info->framebuffer.bytes;
    allocation = kmm_alloc((k_size_t)bytes + GPU_INTEL_GTT_PAGE_SIZE - 1u, KMM_PRIORITY_KERNEL);
    if (allocation == NULL) {
        kshell_printf("gpu: plane-linear has no RAM for %u bytes\n",
                      (unsigned)info->framebuffer.bytes);
        goto out;
    }
    page = (k_u8_t *)(((uintptr_t)allocation + GPU_INTEL_GTT_PAGE_SIZE - 1u) &
                      ~(uintptr_t)(GPU_INTEL_GTT_PAGE_SIZE - 1u));
    physical = port_x86_64_bus_phys(page);
    if (physical == 0u || (physical & (GPU_INTEL_GTT_PAGE_SIZE - 1u)) != 0u) {
        kshell_printf("gpu: plane-linear RAM has no aligned physical address\n");
        goto out;
    }
    {
        k_u32_t i;
        for (i = 0u; i < info->framebuffer.pte_count; i++) {
            if (port_x86_64_bus_phys(page + i * GPU_INTEL_GTT_PAGE_SIZE) !=
                physical + i * GPU_INTEL_GTT_PAGE_SIZE) {
                kshell_printf("gpu: plane-linear RAM is not physically contiguous\n");
                goto out;
            }
        }
    }

    source = gpu_intel_map_aperture(info->framebuffer.surface, info->framebuffer.bytes);
    if (source == NULL) {
        kshell_printf("gpu: plane-linear cannot map the source aperture\n");
        goto out;
    }
    memset(page, 0, info->framebuffer.bytes);
    gpu_intel_detile_x((const k_u8_t *)source, page, width, height, info->framebuffer.pitch,
                       ctl.bpp);

    rc = gpu_intel_ggtt_borrow(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &s_linear.run,
                               physical, &s_linear.saved_gtt);
    if (rc != GPU_INTEL_GGTT_OK) {
        kshell_printf("gpu: plane-linear GGTT install failed (rc=%d)\n", (int)rc);
        goto out;
    }
    run_installed = true;

    kinfo("gpu: plane-linear: writing PLANE_CTL 0x%08x -> 0x%08x, SURF 0x%08x -> 0x%08x",
          (unsigned)control, (unsigned)new_control, (unsigned)surface, (unsigned)new_surface);
    k_logfile_flush();

    if (!gpu_intel_wait_frame(info)) {
        kshell_printf("gpu: plane-linear timed out waiting for vblank\n");
        goto out;
    }
    display_touched = true;
    if (!gpu_intel_plane_program(info, new_control, new_stride, new_surface)) {
        kshell_printf("gpu: plane-linear registers did not read back\n");
        goto out;
    }
    if (!gpu_intel_wait_frame(info) || !gpu_intel_wait_frame(info)) {
        kshell_printf("gpu: plane-linear timed out waiting for scanout\n");
        goto out;
    }
    if ((gpu_intel_mmio_read(live_offset) & GPU_INTEL_PLANE_SURFACE_MASK) != new_surface) {
        kshell_printf("gpu: plane-linear SURFLIVE is not the new surface\n");
        goto out;
    }

    s_linear.active = true;
    s_linear.allocation = allocation;
    s_linear.page = page;
    s_linear.physical = physical;
    s_linear.old_control = control;
    s_linear.old_stride = stride;
    s_linear.old_surface = surface;
    s_linear.old_fb_phys = info->framebuffer.phys;
    s_linear.old_fb_pte_start = info->framebuffer.pte_start;
    s_linear.old_fb_pte_count = info->framebuffer.pte_count;
    s_linear.old_fb_tiling = info->framebuffer.tiling;
    s_linear.old_fb_contiguous = info->framebuffer.contiguous;

    info->plane_control = new_control;
    info->plane_stride = new_stride;
    info->plane_surface = new_surface;
    info->framebuffer.tiling = GPU_INTEL_TILE_LINEAR;
    info->framebuffer.surface = new_surface;
    info->framebuffer.phys = physical;
    info->framebuffer.pte_start = s_linear.run.start;
    info->framebuffer.pte_count = s_linear.run.count;
    info->framebuffer.contiguous = true;

    port_x86_64_fb_set_format(ctl.format == GPU_INTEL_FORMAT_RGB_565 ? FB_FORMAT_RGB565
                                                                     : FB_FORMAT_XRGB8888,
                              ctl.bpp * 8u);
    if (gpu_intel_present_ready(info)) {
        k_fb_ops_register(&s_gpu_intel_present_ops, info);
        kinfo("gpu: Intel page-flip backend ready (%ux%u linear %s)",
              (unsigned)info->framebuffer.width, (unsigned)info->framebuffer.height,
              gpu_intel_format_name(ctl.format));
    }

    kshell_printf("gpu: plane-linear active: linear %s, SURF 0x%08x, SURFLIVE 0x%08x\n",
                  gpu_intel_format_name(ctl.format), (unsigned)new_surface,
                  (unsigned)(gpu_intel_mmio_read(live_offset) & GPU_INTEL_PLANE_SURFACE_MASK));
    ok = true;

out:
    if (!ok && display_touched) {
        if (!gpu_intel_plane_restore(info, control, stride, surface) ||
            !gpu_intel_wait_frame(info) ||
            (gpu_intel_mmio_read(live_offset) & GPU_INTEL_PLANE_SURFACE_MASK) != surface) {
            kerror(
                "gpu: plane-linear rollback is not live; keeping new GGTT mapping and RAM alive");
            keep_buffer = true;
            run_installed = false;
        } else {
            kinfo("gpu: plane-linear: restored original plane");
        }
    }
    if (!ok && run_installed) {
        if (gpu_intel_ggtt_restore(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &s_linear.run,
                                   s_linear.saved_gtt) != GPU_INTEL_GGTT_OK) {
            kerror("gpu: plane-linear could not restore GGTT; keeping RAM alive");
            keep_buffer = true;
        }
    }
    if (!ok && allocation != NULL && !keep_buffer) {
        kmm_free(allocation);
    }
    return ok ? 0 : -1;
}

static int gpu_intel_flip(const struct gpu_intel_info *info, bool confirmed)
{
    struct gpu_intel_gtt_range excluded[16];
    struct gpu_intel_gtt_run run;
    k_size_t excluded_count = 0u;
    k_u32_t control;
    k_u32_t stride;
    k_u32_t surface;
    k_u32_t new_surface;
    k_u32_t live_offset;
    k_u32_t live;
    void *allocation = NULL;
    k_u8_t *page = NULL;
    k_u64_t physical = 0u;
    k_u64_t saved_gtt = 0u;
    volatile k_u8_t *source = NULL;
    bool run_installed = false;
    bool display_touched = false;
    bool plane_restored = false;
    bool keep_buffer = false;
    bool ok = false;
    k_size_t i;
    enum gpu_intel_ggtt_result rc;
    char source_physical[24];
    char target_physical[24];

    if (info->common.probe_result != GPU_PROBE_OK || !info->display_ready ||
        !info->framebuffer_known || info->panel_pipe == GPU_INTEL_PANEL_PIPE_UNKNOWN ||
        s_gtt_entries == NULL) {
        kshell_printf("gpu: flip needs an active, readable Intel display\n");
        return -1;
    }
    if (!gpu_intel_flip_laptop(info)) {
        kshell_printf("gpu: flip is limited to the measured ThinkPad 17aa:225c\n");
        return -1;
    }
    if ((info->plane_surface & ~GPU_INTEL_PLANE_SURFACE_MASK) != 0u ||
        (info->plane_control & GPU_INTEL_RENDER_DECOMPRESSION) != 0u ||
        (info->plane_control & 3u) != 0u) {
        kshell_printf("gpu: flip refuses surface flags, compression or rotation\n");
        return -1;
    }

    control =
        gpu_intel_mmio_read(gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_CTL_BASE));
    stride =
        gpu_intel_mmio_read(gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_STRIDE_BASE));
    surface =
        gpu_intel_mmio_read(gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURF_BASE));
    if (control != info->plane_control || stride != info->plane_stride ||
        surface != info->plane_surface) {
        kshell_printf("gpu: the display changed after the probe; run gpu info again\n");
        return -1;
    }
    if (!gpu_intel_collect_exclusions(info, excluded, K_ARRAY_SIZE(excluded), &excluded_count)) {
        kshell_printf("gpu: flip cannot exclude every live display surface\n");
        return -1;
    }

    rc = gpu_intel_ggtt_find_free(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES,
                                  info->framebuffer.pte_count, excluded, excluded_count, &run);
    if (rc != GPU_INTEL_GGTT_OK) {
        kshell_printf("gpu: no guarded GGTT run for %u pages (rc=%d)\n",
                      (unsigned)info->framebuffer.pte_count, (int)rc);
        return -1;
    }
    new_surface = run.start * GPU_INTEL_GTT_PAGE_SIZE;
    live_offset = gpu_intel_pipe_register(info->panel_pipe, GPU_INTEL_PLANE_SURFLIVE_BASE);

    gpu_intel_address(info->framebuffer.phys, source_physical, sizeof(source_physical));
    kshell_printf("gpu: flip %s: source 0x%s, ggtt 0x%x, %ux%u, pitch %u, %u bytes\n",
                  confirmed ? "plan" : "dry-run", source_physical,
                  (unsigned)info->framebuffer.surface, (unsigned)info->framebuffer.width,
                  (unsigned)info->framebuffer.height, (unsigned)info->framebuffer.pitch,
                  (unsigned)info->framebuffer.bytes);
    kshell_printf("gpu: flip %s: target ggtt 0x%x, slot %u, %u page(s), ctl 0x%08x, "
                  "stride 0x%x\n",
                  confirmed ? "plan" : "dry-run", (unsigned)new_surface, (unsigned)run.start,
                  (unsigned)run.count, (unsigned)control, (unsigned)stride);
    if (!confirmed) {
        kshell_printf("gpu: flip dry-run: nothing written; use gpu flip --yes on the laptop\n");
        return 0;
    }

    allocation = kmm_alloc((k_size_t)info->framebuffer.bytes + GPU_INTEL_GTT_PAGE_SIZE - 1u,
                           KMM_PRIORITY_KERNEL);
    if (allocation == NULL) {
        kshell_printf("gpu: flip has no contiguous RAM for %u bytes\n",
                      (unsigned)info->framebuffer.bytes);
        goto out;
    }
    page = (k_u8_t *)(((uintptr_t)allocation + GPU_INTEL_GTT_PAGE_SIZE - 1u) &
                      ~(uintptr_t)(GPU_INTEL_GTT_PAGE_SIZE - 1u));
    physical = port_x86_64_bus_phys(page);
    if (physical == 0u || (physical & (GPU_INTEL_GTT_PAGE_SIZE - 1u)) != 0u) {
        kshell_printf("gpu: flip RAM has no aligned physical address\n");
        goto out;
    }
    for (i = 0u; i < info->framebuffer.pte_count; i++) {
        if (port_x86_64_bus_phys(page + i * GPU_INTEL_GTT_PAGE_SIZE) !=
            physical + i * GPU_INTEL_GTT_PAGE_SIZE) {
            kshell_printf("gpu: flip RAM is not physically contiguous\n");
            goto out;
        }
    }

    source = gpu_intel_map_aperture(info->framebuffer.surface, info->framebuffer.bytes);
    if (source == NULL) {
        kshell_printf("gpu: flip cannot map the source aperture\n");
        goto out;
    }
    memcpy(page, (const void *)source, info->framebuffer.bytes);
    if (memcmp(page, (const void *)source, info->framebuffer.bytes) != 0) {
        kshell_printf("gpu: flip copy does not match the source\n");
        goto out;
    }

    rc = gpu_intel_ggtt_borrow(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &run, physical,
                               &saved_gtt);
    if (rc != GPU_INTEL_GGTT_OK) {
        kshell_printf("gpu: flip GGTT install failed (rc=%d)\n", (int)rc);
        goto out;
    }
    run_installed = true;

    if (!gpu_intel_wait_frame(info)) {
        kshell_printf("gpu: flip timed out waiting for vblank\n");
        goto out;
    }
    display_touched = true;
    k_logfile_flush();
    if (!gpu_intel_plane_program(info, control, stride, new_surface)) {
        kshell_printf("gpu: flip plane registers did not read back\n");
        goto out;
    }
    if (!gpu_intel_wait_frame(info) || !gpu_intel_wait_frame(info)) {
        kshell_printf("gpu: flip timed out waiting for scanout\n");
        goto out;
    }

    live = gpu_intel_mmio_read(live_offset);
    if ((live & GPU_INTEL_PLANE_SURFACE_MASK) != new_surface ||
        memcmp(page, (const void *)source, info->framebuffer.bytes) != 0) {
        kshell_printf("gpu: flip scanout or pixel copy is not the same\n");
        goto out;
    }
    gpu_intel_address(physical, target_physical, sizeof(target_physical));
    kshell_printf("gpu: flip active at physical 0x%s, SURFLIVE 0x%08x: copy verified\n",
                  target_physical, (unsigned)live);

    if (!gpu_intel_wait_frame(info)) {
        kshell_printf("gpu: flip timed out before restore\n");
        goto out;
    }
    k_logfile_flush();
    plane_restored = gpu_intel_plane_restore(info, control, stride, surface);
    if (!plane_restored) {
        kerror("gpu: flip could not restore the original plane; retrying rollback");
        goto out;
    }
    if (!gpu_intel_wait_frame(info) ||
        (gpu_intel_mmio_read(live_offset) & GPU_INTEL_PLANE_SURFACE_MASK) != surface) {
        kshell_printf("gpu: original surface did not become live again\n");
        plane_restored = false;
        goto out;
    }

    kshell_printf("gpu: flip restored original surface 0x%08x\n", (unsigned)surface);
    ok = true;

out:
    if (display_touched && !plane_restored) {
        k_logfile_flush();
        if (gpu_intel_plane_restore(info, control, stride, surface) && gpu_intel_wait_frame(info) &&
            (gpu_intel_mmio_read(live_offset) & GPU_INTEL_PLANE_SURFACE_MASK) == surface) {
            plane_restored = true;
        } else {
            kerror("gpu: flip rollback is not live; keeping new GGTT mapping and RAM alive");
            keep_buffer = true;
            run_installed = false;
        }
    }
    if (run_installed) {
        if (gpu_intel_ggtt_restore(s_gtt_entries, GPU_INTEL_GTT_APERTURE_ENTRIES, &run,
                                   saved_gtt) != GPU_INTEL_GGTT_OK) {
            kerror("gpu: flip could not restore GGTT slot %u; keeping RAM alive",
                   (unsigned)run.start);
            ok = false;
            keep_buffer = true;
        }
    }
    if (allocation != NULL && !keep_buffer) {
        kmm_free(allocation);
    }
    if (!ok) {
        kshell_printf("gpu: flip aborted; original plane %s\n",
                      plane_restored ? "restored" : "not confirmed");
    }
    return ok ? 0 : -1;
}

static struct gpu_intel_info g_gpu_intel;

static enum gpu_probe_result gpu_intel_finish(struct gpu_intel_info *info, struct gpu_info *out,
                                              enum gpu_probe_result result)
{
    info->common.probe_result = result;
    if (out != NULL) {
        *out = info->common;
    }
    return result;
}

enum gpu_probe_result gpu_intel_probe(const struct port_x86_64_pci_device *pci,
                                      struct gpu_info *out)
{
    struct gpu_intel_info *info = &g_gpu_intel;
    const struct gpu_intel_id *identity;
    k_u8_t power_state;
    k_u16_t command;
    k_u16_t class_code;
    k_u16_t vendor;
    k_u16_t device;
    k_u8_t revision;
    k_u32_t combined_wells;
    k_u32_t edp_conf;
    k_u32_t htotal;
    k_u32_t vtotal;
    k_u32_t width;
    k_u32_t height;

    memset(info, 0, sizeof(*info));
    info->panel_pipe = GPU_INTEL_PANEL_PIPE_UNKNOWN;
    if (pci == NULL) {
        return gpu_intel_finish(info, out, GPU_PROBE_FAIL);
    }

    vendor = port_x86_64_pci_read16(pci, PCI_VENDOR);
    if (vendor != PCI_INTEL_VENDOR) {
        return gpu_intel_finish(info, out, GPU_PROBE_NONE);
    }

    device = port_x86_64_pci_read16(pci, PCI_DEVICE);
    revision = (k_u8_t)(port_x86_64_pci_read16(pci, PCI_REVISION) & 0xFFu);
    info->common.present = true;
    info->common.vendor = vendor;
    info->common.device = device;
    info->common.revision = revision;
    info->common.bus = pci->bus;
    info->common.slot = pci->slot;
    info->common.function = pci->function;
    info->subsystem_vendor = port_x86_64_pci_read16(pci, PCI_SUBSYSTEM_VENDOR);
    info->subsystem_device = port_x86_64_pci_read16(pci, PCI_SUBSYSTEM_DEVICE);

    identity = gpu_intel_id_lookup(vendor, device);
    if (identity == NULL) {
        snprintf(info->common.name, sizeof(info->common.name), "Intel unknown");
        return gpu_intel_finish(info, out, GPU_PROBE_UNKNOWN_DEV);
    }

    info->common.known = true;
    snprintf(info->common.name, sizeof(info->common.name), "%s", identity->name);
    info->common.mmio_size = (k_u32_t)identity->mmio_size;
    info->common.display_gen = identity->display_gen;
    info->common.display_rel = identity->display_rel;
    snprintf(info->common.display_step, sizeof(info->common.display_step), "%s",
             gpu_intel_display_step(device, revision));
    info->pipes = identity->pipes;
    info->transcoders = identity->transcoders;
    info->ports = identity->ports;

    class_code = port_x86_64_pci_read16(pci, PCI_CLASS);
    if (pci->function != 0u || (k_u8_t)(class_code >> 8) != 0x03u ||
        (k_u8_t)(class_code & 0xFFu) != 0x00u) {
        kerror("gpu: Intel PCI identity/class mismatch (GPU_PROBE_FAIL)");
        return gpu_intel_finish(info, out, GPU_PROBE_FAIL);
    }

    if (!gpu_intel_power_state(pci, &power_state)) {
        kinfo("gpu: Intel power state is not readable; MMIO not touched (GPU_PROBE_DEGRADED)");
        return gpu_intel_finish(info, out, GPU_PROBE_DEGRADED);
    }
    if (power_state != PCI_PM_D0) {
        kinfo("gpu: Intel is in D%u; MMIO not touched (GPU_PROBE_DEGRADED)", (unsigned)power_state);
        return gpu_intel_finish(info, out, GPU_PROBE_DEGRADED);
    }

    command = port_x86_64_pci_read16(pci, PCI_COMMAND);
    if ((command & PCI_COMMAND_MEMORY) == 0u) {
        port_x86_64_pci_enable(pci, PCI_COMMAND_MEMORY);
    }

    if (!gpu_intel_map(pci, &info->common.mmio_base)) {
        kerror("gpu: Intel BAR0 is not a mappable memory window (GPU_PROBE_FAIL)");
        return gpu_intel_finish(info, out, GPU_PROBE_FAIL);
    }
    if (!gpu_intel_aperture(pci, info)) {
        kinfo("gpu: Intel BAR2 is not a mappable graphics aperture (GPU_PROBE_DEGRADED)");
        return gpu_intel_finish(info, out, GPU_PROBE_DEGRADED);
    }

    info->fuse_status = gpu_intel_mmio_read(GPU_INTEL_SKL_FUSE_STATUS);
    info->power_well_bios = gpu_intel_mmio_read(GPU_INTEL_HSW_PWR_WELL_CTL1);
    info->power_well_driver = gpu_intel_mmio_read(GPU_INTEL_HSW_PWR_WELL_CTL2);
    if (info->fuse_status == 0xFFFFFFFFu || info->power_well_bios == 0xFFFFFFFFu ||
        info->power_well_driver == 0xFFFFFFFFu) {
        kerror("gpu: Intel MMIO did not answer (GPU_PROBE_FAIL)");
        return gpu_intel_finish(info, out, GPU_PROBE_FAIL);
    }
    info->mmio_responded = true;

    combined_wells = info->power_well_bios | info->power_well_driver;
    info->display_ready = (combined_wells & GPU_INTEL_PW1_STATE) != 0u;
    if (!info->display_ready) {
        return gpu_intel_finish(info, out, GPU_PROBE_DEGRADED);
    }

    info->de_port_isr = gpu_intel_mmio_read(GPU_INTEL_GEN8_DE_PORT_ISR);
    info->de_misc_isr = gpu_intel_mmio_read(GPU_INTEL_GEN8_DE_MISC_ISR);
    if (info->de_port_isr == 0xFFFFFFFFu || info->de_misc_isr == 0xFFFFFFFFu) {
        kerror("gpu: Intel display MMIO did not answer (GPU_PROBE_FAIL)");
        return gpu_intel_finish(info, out, GPU_PROBE_FAIL);
    }

    info->edp_ctl = gpu_intel_mmio_read(GPU_INTEL_TRANS_DDI_FUNC_CTL_EDP);
    if (info->edp_ctl == 0xFFFFFFFFu) {
        kerror("gpu: Intel eDP state did not answer (GPU_PROBE_FAIL)");
        return gpu_intel_finish(info, out, GPU_PROBE_FAIL);
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
                info->common.panel_width = (k_u16_t)width;
                info->common.panel_height = (k_u16_t)height;
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

    if ((info->common.panel_width != 0u || info->common.panel_height != 0u) &&
        !gpu_intel_read_framebuffer(info)) {
        return gpu_intel_finish(info, out, GPU_PROBE_DEGRADED);
    }

    if (gpu_intel_present_ready(info)) {
        struct fb_info fbi;
        k_u8_t plane_format = info->framebuffer.format == GPU_INTEL_FORMAT_RGB_565
                                  ? FB_FORMAT_RGB565
                                  : FB_FORMAT_XRGB8888;
        k_u8_t plane_bpp = info->framebuffer.bpp * 8u;

        if (port_x86_64_fb_info(&fbi) && fbi.bpp == plane_bpp) {
            port_x86_64_fb_set_format(plane_format, plane_bpp);
        } else if (port_x86_64_fb_info(&fbi) && fbi.bpp != plane_bpp) {
            kwarn("gpu: fb format %u/%u bpp does not match plane %s/%u bpp; present may fall back",
                  (unsigned)fbi.format, (unsigned)fbi.bpp,
                  gpu_intel_format_name(info->framebuffer.format), (unsigned)plane_bpp);
        }

        k_fb_ops_register(&s_gpu_intel_present_ops, info);
        kinfo("gpu: Intel page-flip backend ready (%ux%u linear %s)",
              (unsigned)info->framebuffer.width, (unsigned)info->framebuffer.height,
              gpu_intel_format_name(info->framebuffer.format));
    }

    return gpu_intel_finish(info, out, GPU_PROBE_OK);
}

int gpu_intel_command(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "ggtt-test") == 0) {
        if (g_gpu_intel.common.probe_result != GPU_PROBE_OK || !g_gpu_intel.framebuffer_known ||
            !gpu_intel_flip_laptop(&g_gpu_intel)) {
            kshell_printf("gpu: a readable ThinkPad framebuffer is required\n");
            return KSHELL_ERROR;
        }
        return gpu_intel_ggtt_test(&g_gpu_intel) == 0 ? KSHELL_OK : KSHELL_ERROR;
    }

    if ((argc == 2 && strcmp(argv[1], "flip") == 0) ||
        (argc == 3 && strcmp(argv[1], "flip") == 0 && strcmp(argv[2], "--yes") == 0)) {
        if (g_gpu_intel.common.probe_result != GPU_PROBE_OK) {
            kshell_printf("gpu: flip refused: probe result %d\n",
                          (int)g_gpu_intel.common.probe_result);
            return KSHELL_ERROR;
        }
        return gpu_intel_flip(&g_gpu_intel, argc == 3) == 0 ? KSHELL_OK : KSHELL_ERROR;
    }

    if ((argc == 2 && strcmp(argv[1], "plane-linear") == 0) ||
        (argc == 3 && strcmp(argv[1], "plane-linear") == 0 && strcmp(argv[2], "--yes") == 0)) {
        if (g_gpu_intel.common.probe_result != GPU_PROBE_OK) {
            kshell_printf("gpu: plane-linear refused: probe result %d\n",
                          (int)g_gpu_intel.common.probe_result);
            return KSHELL_ERROR;
        }
        return gpu_intel_plane_linear(&g_gpu_intel, argc == 3) == 0 ? KSHELL_OK : KSHELL_ERROR;
    }

    if ((argc == 3 && strcmp(argv[1], "display") == 0 && strcmp(argv[2], "enable") == 0) ||
        (argc == 4 && strcmp(argv[1], "display") == 0 && strcmp(argv[2], "enable") == 0 &&
         strcmp(argv[3], "--yes") == 0)) {
        if (g_gpu_intel.common.probe_result != GPU_PROBE_OK) {
            kshell_printf("gpu: display enable refused: probe result %d\n",
                          (int)g_gpu_intel.common.probe_result);
            return KSHELL_ERROR;
        }
        return gpu_intel_display_enable(&g_gpu_intel, argc == 4) == 0 ? KSHELL_OK : KSHELL_ERROR;
    }

    kshell_printf(
        "usage: gpu [info|ggtt-test|flip [--yes]|plane-linear [--yes]|display enable [--yes]]\n");
    return KSHELL_USAGE;
}
