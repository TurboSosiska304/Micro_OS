/*
 * Micro OS — decoding of the flat gen8 global graphics page table.
 *
 * Facts from drivers/gpu/drm/i915/gt/intel_gtt.h,
 * drivers/gpu/drm/i915/gt/intel_ggtt.c,
 * drivers/gpu/drm/i915/display/intel_fb.c and
 * drivers/gpu/drm/i915/display/skl_universal_plane.c (GPL-2.0, Linux v6.12);
 * code written here for Micro OS (MIT).
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_INTEL_GGTT_H
#define MICRO_OS_PORT_X86_64_GPU_INTEL_GGTT_H

#include "kernel/types.h"

#define GPU_INTEL_GTT_PAGE_SIZE 4096u
#define GPU_INTEL_GTT_ENTRY_SIZE 8u
#define GPU_INTEL_GTT_PRESENT (1ULL << 0)
#define GPU_INTEL_GTT_ADDRESS_MASK 0x000FFFFFFFFFF000ULL

#define GPU_INTEL_PLANE_ENABLE (1u << 31)
#define GPU_INTEL_PLANE_FORMAT_MASK (0xFu << 24)
#define GPU_INTEL_PLANE_TILE_MASK (0x7u << 10)
#define GPU_INTEL_PLANE_STRIDE_MASK 0xFFFu
#define GPU_INTEL_PLANE_SURFACE_MASK 0xFFFFF000u

#define GPU_INTEL_FORMAT_XRGB_2101010 2u
#define GPU_INTEL_FORMAT_XRGB_8888 4u
#define GPU_INTEL_FORMAT_RGB_565 14u

#define GPU_INTEL_TILE_LINEAR 0u
#define GPU_INTEL_TILE_X 1u
#define GPU_INTEL_TILE_Y 4u
#define GPU_INTEL_TILE_YF 5u

struct gpu_intel_fb {
    k_u64_t surface;   /* byte offset in the graphics aperture */
    k_u64_t phys;      /* physical address of the first page */
    k_u32_t width;     /* visible pixels */
    k_u32_t height;    /* visible scanlines */
    k_u32_t pitch;     /* bytes per scanline */
    k_u32_t bytes;     /* pitch times the tile-aligned height */
    k_u32_t pte_start; /* first global page-table entry */
    k_u32_t pte_count; /* entries occupied by the surface */
    k_u8_t bpp;
    k_u8_t format;
    k_u8_t tiling;
    bool contiguous;
};

struct gpu_intel_gtt_range {
    k_u32_t start;
    k_u32_t count;
};

struct gpu_intel_gtt_run {
    k_u32_t start;
    k_u32_t count;
    k_u64_t original;
};

enum gpu_intel_ggtt_result {
    GPU_INTEL_GGTT_OK = 0,
    GPU_INTEL_GGTT_INVALID = -1,
    GPU_INTEL_GGTT_UNMAPPED = -2,
    GPU_INTEL_GGTT_TOO_LARGE = -3,
    GPU_INTEL_GGTT_NO_FREE = -4,
};

/*
 * Decode a primary-plane surface through the flat GGTT. Gen8 GGTT entries are
 * one 64-bit PTE per 4 KiB page; the four-level PDP/PDE walk belongs to PPGTT,
 * not to this global table. No hardware is touched here.
 */
enum gpu_intel_ggtt_result gpu_intel_ggtt_decode_fb(const volatile k_u64_t *entries,
                                                    k_size_t entry_count, k_u32_t plane_surface,
                                                    k_u32_t plane_stride, k_u32_t plane_control,
                                                    k_u32_t width, k_u32_t height,
                                                    struct gpu_intel_fb *out);

/*
 * Find a run which still has the same valid scratch PTE on both guard entries.
 * The caller excludes every live display surface. This is deliberately more
 * conservative than a zero test: unused gen8 GGTT slots normally point at the
 * driver's scratch page, and a guard catches the edge of an allocation.
 */
enum gpu_intel_ggtt_result gpu_intel_ggtt_find_free(const volatile k_u64_t *entries,
                                                    k_size_t entry_count, k_u32_t needed,
                                                    const struct gpu_intel_gtt_range *excluded,
                                                    k_size_t excluded_count,
                                                    struct gpu_intel_gtt_run *out);

/* Borrow one run and restore it byte-for-byte; both verify the PTE readback. */
enum gpu_intel_ggtt_result gpu_intel_ggtt_borrow(volatile k_u64_t *entries, k_size_t entry_count,
                                                 const struct gpu_intel_gtt_run *run,
                                                 k_u64_t physical, k_u64_t *saved);
enum gpu_intel_ggtt_result gpu_intel_ggtt_restore(volatile k_u64_t *entries, k_size_t entry_count,
                                                  const struct gpu_intel_gtt_run *run,
                                                  k_u64_t saved);

#endif /* MICRO_OS_PORT_X86_64_GPU_INTEL_GGTT_H */
