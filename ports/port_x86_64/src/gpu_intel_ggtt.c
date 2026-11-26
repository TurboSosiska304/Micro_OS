/*
 * Micro OS — decoding of the flat gen8 global graphics page table.
 *
 * Facts from drivers/gpu/drm/i915/gt/intel_gtt.h,
 * drivers/gpu/drm/i915/gt/intel_ggtt.c and
 * drivers/gpu/drm/i915/display/skl_universal_plane.c (GPL-2.0, Linux v6.12);
 * code written here for Micro OS (MIT).
 */
#include "gpu_intel_ggtt.h"

static bool gpu_intel_format_bpp(k_u8_t format, k_u8_t *bpp)
{
    switch (format) {
    case GPU_INTEL_FORMAT_RGB_565:
        *bpp = 2u;
        return true;
    case GPU_INTEL_FORMAT_XRGB_2101010:
    case GPU_INTEL_FORMAT_XRGB_8888:
        *bpp = 4u;
        return true;
    default:
        return false;
    }
}

static bool gpu_intel_stride_unit(k_u8_t tiling, k_u8_t bpp, k_u32_t *unit, k_u32_t *tile_height)
{
    switch (tiling) {
    case GPU_INTEL_TILE_LINEAR:
        *unit = 64u;
        *tile_height = 1u;
        return true;
    case GPU_INTEL_TILE_X:
    case GPU_INTEL_TILE_Y:
        *unit = 512u;
        *tile_height = GPU_INTEL_GTT_PAGE_SIZE / *unit;
        return true;
    case GPU_INTEL_TILE_YF:
        if (bpp != 2u && bpp != 4u) {
            return false;
        }
        *unit = 128u;
        *tile_height = GPU_INTEL_GTT_PAGE_SIZE / *unit;
        return true;
    default:
        return false;
    }
}

enum gpu_intel_ggtt_result gpu_intel_ggtt_decode_fb(const volatile k_u64_t *entries,
                                                    k_size_t entry_count, k_u32_t plane_surface,
                                                    k_u32_t plane_stride, k_u32_t plane_control,
                                                    k_u32_t width, k_u32_t height,
                                                    struct gpu_intel_fb *out)
{
    struct gpu_intel_fb fb;
    k_u8_t format;
    k_u8_t tiling;
    k_u8_t bpp;
    k_u32_t unit;
    k_u32_t tile_height;
    k_u32_t aligned_height;
    k_u32_t pte_start;
    k_u32_t pte_count;
    k_u64_t bytes;
    k_u64_t previous = 0u;
    k_size_t i;

    if (out == NULL || entries == NULL || width == 0u || height == 0u || width > 16384u ||
        height > 16384u || (plane_control & GPU_INTEL_PLANE_ENABLE) == 0u) {
        return GPU_INTEL_GGTT_INVALID;
    }

    format = (k_u8_t)((plane_control & GPU_INTEL_PLANE_FORMAT_MASK) >> 24);
    tiling = (k_u8_t)((plane_control & GPU_INTEL_PLANE_TILE_MASK) >> 10);
    if (!gpu_intel_format_bpp(format, &bpp) ||
        !gpu_intel_stride_unit(tiling, bpp, &unit, &tile_height)) {
        return GPU_INTEL_GGTT_INVALID;
    }

    fb.surface = (k_u64_t)(plane_surface & GPU_INTEL_PLANE_SURFACE_MASK);
    fb.width = width;
    fb.height = height;
    fb.bpp = bpp;
    fb.format = format;
    fb.tiling = tiling;
    fb.pitch = (plane_stride & GPU_INTEL_PLANE_STRIDE_MASK) * unit;
    if (fb.pitch == 0u || fb.pitch > (1u << 20)) {
        return GPU_INTEL_GGTT_INVALID;
    }

    aligned_height = (height + tile_height - 1u) & ~(tile_height - 1u);
    bytes = (k_u64_t)fb.pitch * (k_u64_t)aligned_height;
    if (bytes == 0u || bytes > 256u * 1024u * 1024u) {
        return GPU_INTEL_GGTT_TOO_LARGE;
    }
    fb.bytes = (k_u32_t)bytes;

    pte_start = (k_u32_t)(fb.surface / GPU_INTEL_GTT_PAGE_SIZE);
    pte_count = (k_u32_t)((bytes + GPU_INTEL_GTT_PAGE_SIZE - 1u) / GPU_INTEL_GTT_PAGE_SIZE);
    if ((k_u64_t)pte_start + (k_u64_t)pte_count > (k_u64_t)entry_count) {
        return GPU_INTEL_GGTT_UNMAPPED;
    }

    fb.pte_start = pte_start;
    fb.pte_count = pte_count;
    fb.contiguous = true;
    for (i = 0; i < pte_count; i++) {
        k_u64_t entry = entries[pte_start + i];
        k_u64_t physical;

        if ((entry & GPU_INTEL_GTT_PRESENT) == 0u) {
            return GPU_INTEL_GGTT_UNMAPPED;
        }
        physical = entry & GPU_INTEL_GTT_ADDRESS_MASK;
        if (physical == 0u) {
            return GPU_INTEL_GGTT_UNMAPPED;
        }
        if (i == 0u) {
            fb.phys = physical;
        } else if (physical != previous + GPU_INTEL_GTT_PAGE_SIZE) {
            fb.contiguous = false;
        }
        previous = physical;
    }

    *out = fb;
    return GPU_INTEL_GGTT_OK;
}
