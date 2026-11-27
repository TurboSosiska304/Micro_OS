/*
 * Micro OS — the x86-64 port: AMD Polaris (GMC v8.0) register facts.
 *
 * Facts from drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c (MIT, Linux v6.12).
 * Code written here.
 * Facts from drivers/gpu/drm/amd/include/asic_reg/gmc/gmc_8_1_d.h
 *   (MIT, Linux v6.12). Code written here.
 * Facts from drivers/gpu/drm/amd/include/asic_reg/gmc/gmc_8_1_sh_mask.h
 *   (MIT, Linux v6.12). Code written here.
 * Facts from drivers/gpu/drm/amd/include/asic_reg/bif/bif_5_0_d.h
 *   (MIT, Linux v6.12). Code written here.
 * Facts from drivers/gpu/drm/amd/include/asic_reg/gca/gfx_8_0_d.h
 *   (MIT, Linux v6.12). Code written here.
 * Facts from drivers/gpu/drm/amd/include/asic_reg/gca/gfx_8_0_sh_mask.h
 *   (MIT, Linux v6.12). Code written here.
 * Facts from drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_0_d.h
 *   (MIT, Linux v6.12). Code written here.
 * Facts from drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_0_sh_mask.h
 *   (MIT, Linux v6.12). Code written here.
 *
 * Only register indices, field masks and pure decode helpers are here. No
 * MMIO access, no PCI, no display control writes.
 */
#ifndef MICRO_OS_PORT_X86_64_GPU_AMD_H
#define MICRO_OS_PORT_X86_64_GPU_AMD_H

#include "kernel/types.h"

/* BIF register: VRAM size in MiB (Polaris/VI). */
#define GPU_AMD_MMCONFIG_MEMSIZE 0x150au

/* GMC v8.1 register indices (dword indices, MMIO offset = index * 4). */
#define GPU_AMD_MMMC_SHARED_CHMAP 0x801u
#define GPU_AMD_MMMC_ARB_RAMCFG 0x9d8u
#define GPU_AMD_MMMC_VM_MX_L1_TLB_CNTL 0x819u
#define GPU_AMD_MMMC_SEQ_MISC0 0xa80u

/* GC register indices for SE/SH topology (read-only). */
#define GPU_AMD_MMGB_ADDR_CONFIG 0x263eu
#define GPU_AMD_MMCC_GC_SHADER_ARRAY_CONFIG 0x226fu
#define GPU_AMD_MMGC_USER_SHADER_ARRAY_CONFIG 0x2270u

/* MC_SHARED_CHMAP.NOOFCHAN: mask 0xf000, shift 12. */
#define GPU_AMD_MC_SHARED_CHMAP__NOOFCHAN_MASK 0x0000f000u
#define GPU_AMD_MC_SHARED_CHMAP__NOOFCHAN_SHIFT 12u

/* MC_ARB_RAMCFG.CHANSIZE: mask 0x100, shift 8 (0=32-bit, 1=64-bit). */
#define GPU_AMD_MC_ARB_RAMCFG__CHANSIZE_MASK 0x00000100u
#define GPU_AMD_MC_ARB_RAMCFG__CHANSIZE_SHIFT 8u

/* GB_ADDR_CONFIG.NUM_PIPES: mask 0x7, shift 0. */
#define GPU_AMD_GB_ADDR_CONFIG__NUM_PIPES_MASK 0x00000007u
#define GPU_AMD_GB_ADDR_CONFIG__NUM_PIPES_SHIFT 0u
/* GB_ADDR_CONFIG.NUM_SHADER_ENGINES: mask 0x3000, shift 12. */
#define GPU_AMD_GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK 0x00003000u
#define GPU_AMD_GB_ADDR_CONFIG__NUM_SHADER_ENGINES_SHIFT 12u

/* GC_USER_SHADER_ARRAY_CONFIG.INACTIVE_CUS: mask 0xffff0000, shift 16. */
#define GPU_AMD_GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK 0xffff0000u
#define GPU_AMD_GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS_SHIFT 16u

struct gpu_amd_mc_info {
    k_u32_t config_memsize_mb;
    k_u64_t vram_mc_bytes;
    k_u8_t num_channels;
    k_u8_t chansize_bits;
    k_u32_t mc_arb_ramcfg;
    k_u32_t mc_shared_chmap;
    k_u32_t mc_vm_mx_l1_tlb_cntl;
    k_u32_t mc_seq_misc0;
    k_u32_t gb_addr_config;
    k_u8_t num_se;
    k_u8_t num_pipes;
};

static inline k_u32_t gpu_amd_reg_field(k_u32_t value, k_u32_t mask, k_u32_t shift)
{
    return (value & mask) >> shift;
}

/*
 * Decode CONFIG_MEMSIZE (MiB). Some boards report garbage in upper 16 bits;
 * Linux gmc_v8_0.c clamps to the lower 16 bits when the upper half is set.
 * Returns 0 when the register reads as 0.
 */
static inline k_u32_t gpu_amd_decode_memsize_mb(k_u32_t config_memsize)
{
    k_u32_t v = config_memsize;

    if ((v & 0xffff0000u) != 0u) {
        if ((v & 0x0000ffffu) != 0u) {
            v &= 0x0000ffffu;
        } else {
            v = 0u;
        }
    }

    return v;
}

/* Decode MC_SHARED_CHMAP.NOOFCHAN to channel count, as in gmc_v8_0_mc_init. */
static inline k_u8_t gpu_amd_decode_noofchan(k_u32_t shared_chmap)
{
    k_u32_t noofchan = gpu_amd_reg_field(shared_chmap, GPU_AMD_MC_SHARED_CHMAP__NOOFCHAN_MASK,
                                         GPU_AMD_MC_SHARED_CHMAP__NOOFCHAN_SHIFT);

    switch (noofchan) {
    case 0:
    default:
        return 1u;
    case 1:
        return 2u;
    case 2:
        return 4u;
    case 3:
        return 8u;
    case 4:
        return 3u;
    case 5:
        return 6u;
    case 6:
        return 10u;
    case 7:
        return 12u;
    case 8:
        return 16u;
    }
}

/* Decode MC_ARB_RAMCFG.CHANSIZE: 0=32-bit, 1=64-bit. */
static inline k_u8_t gpu_amd_decode_chansize(k_u32_t arb_ramcfg)
{
    k_u32_t bit = gpu_amd_reg_field(arb_ramcfg, GPU_AMD_MC_ARB_RAMCFG__CHANSIZE_MASK,
                                    GPU_AMD_MC_ARB_RAMCFG__CHANSIZE_SHIFT);
    return bit != 0u ? 64u : 32u;
}

/* DCE 11 (Polaris) scanout register facts (dword indices). */
#define GPU_AMD_MMDCP_GRPH_ENABLE 0x1a00u
#define GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS 0x1a04u
#define GPU_AMD_MMDCP_GRPH_SECONDARY_SURFACE_ADDRESS 0x1a05u
#define GPU_AMD_MMDCP_GRPH_PITCH 0x1a06u
#define GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH 0x1a07u
#define GPU_AMD_MMDCP_GRPH_SECONDARY_SURFACE_ADDRESS_HIGH 0x1a08u
#define GPU_AMD_MMDCP_GRPH_Y_END 0x1a0eu

#define GPU_AMD_GRPH_ENABLE__GRPH_ENABLE_MASK 0x00000001u
#define GPU_AMD_GRPH_PITCH__GRPH_PITCH_MASK 0x00007fffu
#define GPU_AMD_GRPH_Y_END__GRPH_Y_END_MASK 0x00003fffu

/* CRTC status (read-only): used to wait for vblank before/after a flip.
 * Facts from drivers/gpu/drm/amd/include/asic_reg/dce/dce_11_0_d.h and
 * dce_11_0_sh_mask.h (MIT, Linux v6.12). Code written here. */
#define GPU_AMD_MMCRTC_STATUS 0x1ba3u
#define GPU_AMD_CRTC_STATUS__CRTC_V_BLANK_MASK 0x00000001u

/* DCP/CRTC pipe register stride offsets (instances 0-5). */
#define GPU_AMD_DCE_PIPE_COUNT 6u

struct gpu_amd_scanout_region {
    k_u8_t pipe;
    bool primary;
    bool enabled;
    k_u64_t address;
    k_u32_t pitch_pixels;
    k_u32_t height;
    k_u64_t size_bytes;
};

static inline k_u32_t gpu_amd_dcp_offset(k_u8_t pipe)
{
    static const k_u32_t off[GPU_AMD_DCE_PIPE_COUNT] = {0x0u,    0x200u,  0x400u,
                                                        0x2600u, 0x2800u, 0x2a00u};
    if (pipe >= GPU_AMD_DCE_PIPE_COUNT) {
        return 0u;
    }
    return off[pipe];
}

static inline bool gpu_amd_ranges_overlap(k_u64_t a_start, k_u64_t a_size, k_u64_t b_start,
                                          k_u64_t b_size)
{
    k_u64_t a_end;
    k_u64_t b_end;

    if (a_size == 0u || b_size == 0u) {
        return false;
    }
    a_end = a_start + a_size;
    b_end = b_start + b_size;
    if (a_end < a_start) {
        a_end = ~(k_u64_t)0u;
    }
    if (b_end < b_start) {
        b_end = ~(k_u64_t)0u;
    }
    return a_start < b_end && b_start < a_end;
}

static inline k_u64_t gpu_amd_scanout_size(k_u32_t pitch_pixels, k_u32_t y_end)
{
    k_u64_t h = (k_u64_t)(y_end & GPU_AMD_GRPH_Y_END__GRPH_Y_END_MASK) + 1ull;
    k_u64_t p = (k_u64_t)(pitch_pixels & GPU_AMD_GRPH_PITCH__GRPH_PITCH_MASK);
    k_u64_t bytes;

    if (p == 0u || h == 0u) {
        return 0u;
    }
    /* Conservative: assume 4 bytes/pixel. */
    bytes = p * h * 4ull;
    if (bytes < (1ull * 1024ull * 1024ull)) {
        bytes = 1ull * 1024ull * 1024ull;
    }
    if (bytes > (256ull * 1024ull * 1024ull)) {
        bytes = 256ull * 1024ull * 1024ull;
    }
    return bytes;
}

#define GPU_AMD_VRAM_TEST_BYTES 4096u
#define GPU_AMD_VRAM_TEST_MIN_OFFSET (16ull * 1024ull * 1024ull)
#define GPU_AMD_VRAM_TEST_CANDIDATE (4ull * 1024ull * 1024ull)

static inline bool gpu_amd_scanout_overlaps(k_u64_t start, k_u64_t size,
                                            const struct gpu_amd_scanout_region *regions, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (regions[i].enabled &&
            gpu_amd_ranges_overlap(start, size, regions[i].address, regions[i].size_bytes)) {
            return true;
        }
    }
    return false;
}

static inline k_u64_t gpu_amd_choose_test_offset(k_u64_t vram_bytes,
                                                 const struct gpu_amd_scanout_region *regions,
                                                 int count)
{
    k_u64_t candidate;
    k_u64_t step = 4ull * 1024ull * 1024ull;
    int tries = 0;

    if (vram_bytes < GPU_AMD_VRAM_TEST_MIN_OFFSET + GPU_AMD_VRAM_TEST_BYTES) {
        return 0u;
    }

    if (vram_bytes >= GPU_AMD_VRAM_TEST_CANDIDATE + GPU_AMD_VRAM_TEST_BYTES) {
        candidate = vram_bytes - GPU_AMD_VRAM_TEST_CANDIDATE;
        candidate &= ~((k_u64_t)GPU_AMD_VRAM_TEST_BYTES - 1ull);
        for (tries = 0; tries < 16 && candidate >= GPU_AMD_VRAM_TEST_MIN_OFFSET; tries++) {
            if (!gpu_amd_scanout_overlaps(candidate, GPU_AMD_VRAM_TEST_BYTES, regions, count)) {
                return candidate;
            }
            if (candidate < step) {
                break;
            }
            candidate -= step;
        }
    }

    return 0u;
}

/*
 * A simple VRAM block manager for the BAR0 aperture. Original work for
 * Micro OS. The layout is informed by the amdgpu VRAM manager idea of
 * reserved regions and a free pool (drivers/gpu/drm/amd/amdgpu/amdgpu_vram_mgr.c,
 * MIT, Linux v6.12) but no code is copied from it.
 *
 * Block size is 64 KiB: 8 GiB / 64 KiB = 131072 units, so a bitmap would be
 * 16 KiB, but variable-size allocations and coalescing are simpler with a free
 * extent list. 4 KiB would create too many units to track statically.
 */
#define GPU_AMD_VRAM_BLOCK_SIZE (64ull * 1024ull)
#define GPU_AMD_VRAM_MAX_ALLOCS 64
#define GPU_AMD_VRAM_MAX_FREE 32
#define GPU_AMD_VRAM_RESERVED_END GPU_AMD_VRAM_TEST_MIN_OFFSET

enum gpu_amd_vram_err {
    GPU_AMD_VRAM_OK = 0,
    GPU_AMD_VRAM_ERR_INIT = -1,     /* bad geometry / not initialized */
    GPU_AMD_VRAM_ERR_RANGE = -2,    /* size zero, too large, or out of pool */
    GPU_AMD_VRAM_ERR_NO_SPACE = -3, /* no free extent fits */
    GPU_AMD_VRAM_ERR_NO_DESC = -4,  /* allocation/free-extent table full */
    GPU_AMD_VRAM_ERR_BAD_ID = -5,   /* unknown or already free id */
    GPU_AMD_VRAM_ERR_DOUBLE_FREE = -6,
    GPU_AMD_VRAM_ERR_RESERVED = -7,  /* overlaps scanout or reserved area */
    GPU_AMD_VRAM_ERR_NOT_MAPPED = -8 /* BAR0 aperture not mapped for I/O */
};

struct gpu_amd_vram_block {
    k_u32_t id; /* 0 = unused slot */
    k_u64_t offset;
    k_u64_t size;
    k_u32_t pattern;
    bool filled;
};

struct gpu_amd_vram_extent {
    k_u64_t offset;
    k_u64_t size;
};

struct gpu_amd_vram_mgr {
    bool ready;
    k_u64_t vram_size;
    k_u64_t reserved_end;
    struct gpu_amd_scanout_region scanouts[GPU_AMD_DCE_PIPE_COUNT * 2];
    int scanout_count;
    struct gpu_amd_vram_extent free_extents[GPU_AMD_VRAM_MAX_FREE];
    int free_count;
    struct gpu_amd_vram_block allocs[GPU_AMD_VRAM_MAX_ALLOCS];
    k_u32_t next_id;
};

static inline const char *gpu_amd_vram_err_name(int err)
{
    switch (err) {
    case GPU_AMD_VRAM_OK:
        return "ok";
    case GPU_AMD_VRAM_ERR_INIT:
        return "not initialized";
    case GPU_AMD_VRAM_ERR_RANGE:
        return "invalid size/range";
    case GPU_AMD_VRAM_ERR_NO_SPACE:
        return "no space";
    case GPU_AMD_VRAM_ERR_NO_DESC:
        return "descriptor table full";
    case GPU_AMD_VRAM_ERR_BAD_ID:
        return "bad id";
    case GPU_AMD_VRAM_ERR_DOUBLE_FREE:
        return "double free";
    case GPU_AMD_VRAM_ERR_RESERVED:
        return "reserved/scanout overlap";
    case GPU_AMD_VRAM_ERR_NOT_MAPPED:
        return "BAR0 not mapped";
    default:
        return "unknown error";
    }
}

static inline k_u64_t gpu_amd_vram_align_up(k_u64_t v, k_u64_t align)
{
    k_u64_t mask;

    if (align == 0u) {
        return v;
    }
    mask = align - 1ull;
    if (v > ~(k_u64_t)0u - mask) {
        return 0u;
    }
    return (v + mask) & ~mask;
}

static inline bool gpu_amd_vram_reserved_overlap(const struct gpu_amd_vram_mgr *mgr, k_u64_t start,
                                                 k_u64_t size)
{
    if (mgr == NULL || size == 0u) {
        return true;
    }
    if (start < mgr->reserved_end) {
        return true;
    }
    if (gpu_amd_scanout_overlaps(start, size, mgr->scanouts, mgr->scanout_count)) {
        return true;
    }
    return false;
}

static inline void gpu_amd_vram_mgr_reset(struct gpu_amd_vram_mgr *mgr)
{
    int i;

    if (mgr == NULL) {
        return;
    }
    mgr->ready = false;
    mgr->vram_size = 0u;
    mgr->reserved_end = GPU_AMD_VRAM_RESERVED_END;
    mgr->scanout_count = 0;
    mgr->free_count = 0;
    mgr->next_id = 1u;
    for (i = 0; i < GPU_AMD_VRAM_MAX_FREE; i++) {
        mgr->free_extents[i].offset = 0u;
        mgr->free_extents[i].size = 0u;
    }
    for (i = 0; i < GPU_AMD_VRAM_MAX_ALLOCS; i++) {
        mgr->allocs[i].id = 0u;
        mgr->allocs[i].offset = 0u;
        mgr->allocs[i].size = 0u;
        mgr->allocs[i].pattern = 0u;
        mgr->allocs[i].filled = false;
    }
    for (i = 0; i < (int)(GPU_AMD_DCE_PIPE_COUNT * 2u); i++) {
        mgr->scanouts[i].pipe = 0u;
        mgr->scanouts[i].primary = false;
        mgr->scanouts[i].enabled = false;
        mgr->scanouts[i].address = 0u;
        mgr->scanouts[i].pitch_pixels = 0u;
        mgr->scanouts[i].height = 0u;
        mgr->scanouts[i].size_bytes = 0u;
    }
}

static inline int gpu_amd_vram_add_free_extent(struct gpu_amd_vram_mgr *mgr, k_u64_t offset,
                                               k_u64_t size)
{
    int i;
    int pos;
    k_u64_t end;

    if (mgr == NULL || size == 0u) {
        return GPU_AMD_VRAM_ERR_INIT;
    }
    offset = gpu_amd_vram_align_up(offset, GPU_AMD_VRAM_BLOCK_SIZE);
    if (offset == 0u && size != 0u && mgr->reserved_end != 0u) {
        /* overflow in align_up returns 0; treat as bad range unless genuinely 0 */
    }
    end = offset + size;
    if (end < offset || offset >= mgr->vram_size) {
        return GPU_AMD_VRAM_ERR_RANGE;
    }
    if (end > mgr->vram_size) {
        end = mgr->vram_size;
    }
    /* Round start up, end down to block size. */
    offset = gpu_amd_vram_align_up(offset, GPU_AMD_VRAM_BLOCK_SIZE);
    end &= ~(GPU_AMD_VRAM_BLOCK_SIZE - 1ull);
    if (end <= offset) {
        return GPU_AMD_VRAM_OK;
    }

    /* Insert sorted by offset and merge with neighbors. */
    pos = mgr->free_count;
    for (i = 0; i < mgr->free_count; i++) {
        if (offset < mgr->free_extents[i].offset) {
            pos = i;
            break;
        }
    }
    if (mgr->free_count >= GPU_AMD_VRAM_MAX_FREE) {
        return GPU_AMD_VRAM_ERR_NO_DESC;
    }
    for (i = mgr->free_count; i > pos; i--) {
        mgr->free_extents[i] = mgr->free_extents[i - 1];
    }
    mgr->free_extents[pos].offset = offset;
    mgr->free_extents[pos].size = end - offset;
    mgr->free_count++;

    /* Merge adjacent/overlapping extents. */
    i = 0;
    while (i < mgr->free_count - 1) {
        k_u64_t i_end = mgr->free_extents[i].offset + mgr->free_extents[i].size;
        k_u64_t n_off = mgr->free_extents[i + 1].offset;
        k_u64_t n_end = mgr->free_extents[i + 1].offset + mgr->free_extents[i + 1].size;
        if (i_end >= n_off) {
            if (n_end > i_end) {
                mgr->free_extents[i].size = n_end - mgr->free_extents[i].offset;
            }
            for (int j = i + 1; j < mgr->free_count - 1; j++) {
                mgr->free_extents[j] = mgr->free_extents[j + 1];
            }
            mgr->free_count--;
            continue;
        }
        i++;
    }

    return GPU_AMD_VRAM_OK;
}

static inline int gpu_amd_vram_subtract_range(struct gpu_amd_vram_mgr *mgr, k_u64_t start,
                                              k_u64_t size)
{
    int i = 0;
    k_u64_t end;

    if (mgr == NULL || size == 0u) {
        return GPU_AMD_VRAM_OK;
    }
    end = start + size;
    if (end < start) {
        end = ~(k_u64_t)0u;
    }

    while (i < mgr->free_count) {
        struct gpu_amd_vram_extent *e = &mgr->free_extents[i];
        k_u64_t e_end = e->offset + e->size;
        k_u64_t left_size;
        k_u64_t right_off;
        k_u64_t right_size;

        if (end <= e->offset || start >= e_end) {
            i++;
            continue;
        }

        left_size = (start > e->offset) ? (start - e->offset) : 0u;
        right_off = (end > e->offset) ? end : e->offset;
        right_size = (e_end > right_off) ? (e_end - right_off) : 0u;

        if (left_size == 0u && right_size == 0u) {
            for (int j = i; j < mgr->free_count - 1; j++) {
                mgr->free_extents[j] = mgr->free_extents[j + 1];
            }
            mgr->free_count--;
            continue;
        }

        if (left_size != 0u && right_size != 0u) {
            if (mgr->free_count >= GPU_AMD_VRAM_MAX_FREE) {
                return GPU_AMD_VRAM_ERR_NO_DESC;
            }
            for (int j = mgr->free_count; j > i + 1; j--) {
                mgr->free_extents[j] = mgr->free_extents[j - 1];
            }
            mgr->free_extents[i].size = left_size;
            mgr->free_extents[i + 1].offset = right_off;
            mgr->free_extents[i + 1].size = right_size;
            mgr->free_count++;
            i += 2;
            continue;
        }

        if (left_size != 0u) {
            e->size = left_size;
        } else {
            e->offset = right_off;
            e->size = right_size;
        }
        i++;
    }

    return GPU_AMD_VRAM_OK;
}

static inline int gpu_amd_vram_mgr_init(struct gpu_amd_vram_mgr *mgr, k_u64_t vram_size,
                                        const struct gpu_amd_scanout_region *regions, int count)
{
    k_u64_t pool_start;
    k_u64_t pool_end;
    int i;
    int rc;

    if (mgr == NULL || vram_size < GPU_AMD_VRAM_RESERVED_END + GPU_AMD_VRAM_BLOCK_SIZE) {
        if (mgr != NULL) {
            gpu_amd_vram_mgr_reset(mgr);
        }
        return GPU_AMD_VRAM_ERR_INIT;
    }

    gpu_amd_vram_mgr_reset(mgr);
    mgr->vram_size = vram_size;
    mgr->reserved_end = GPU_AMD_VRAM_RESERVED_END;
    mgr->scanout_count = 0;
    if (regions != NULL && count > 0) {
        for (i = 0; i < count && mgr->scanout_count < (int)(GPU_AMD_DCE_PIPE_COUNT * 2u); i++) {
            if (!regions[i].enabled || regions[i].size_bytes == 0u) {
                continue;
            }
            mgr->scanouts[mgr->scanout_count] = regions[i];
            mgr->scanout_count++;
        }
    }

    pool_start = gpu_amd_vram_align_up(mgr->reserved_end, GPU_AMD_VRAM_BLOCK_SIZE);
    pool_end = vram_size & ~(GPU_AMD_VRAM_BLOCK_SIZE - 1ull);
    if (pool_end <= pool_start) {
        return GPU_AMD_VRAM_ERR_NO_SPACE;
    }

    mgr->free_extents[0].offset = pool_start;
    mgr->free_extents[0].size = pool_end - pool_start;
    mgr->free_count = 1;

    for (i = 0; i < mgr->scanout_count; i++) {
        rc =
            gpu_amd_vram_subtract_range(mgr, mgr->scanouts[i].address, mgr->scanouts[i].size_bytes);
        if (rc != GPU_AMD_VRAM_OK) {
            gpu_amd_vram_mgr_reset(mgr);
            return rc;
        }
    }

    mgr->ready = mgr->free_count > 0;
    return mgr->ready ? GPU_AMD_VRAM_OK : GPU_AMD_VRAM_ERR_NO_SPACE;
}

static inline struct gpu_amd_vram_block *gpu_amd_vram_find(struct gpu_amd_vram_mgr *mgr, k_u32_t id)
{
    int i;

    if (mgr == NULL || id == 0u) {
        return NULL;
    }
    for (i = 0; i < GPU_AMD_VRAM_MAX_ALLOCS; i++) {
        if (mgr->allocs[i].id == id) {
            return &mgr->allocs[i];
        }
    }
    return NULL;
}

static inline int gpu_amd_vram_alloc(struct gpu_amd_vram_mgr *mgr, k_u64_t size_bytes,
                                     k_u32_t *out_id, k_u64_t *out_offset)
{
    k_u64_t size;
    int slot = -1;
    int i;

    if (out_id != NULL) {
        *out_id = 0u;
    }
    if (out_offset != NULL) {
        *out_offset = 0u;
    }
    if (mgr == NULL || !mgr->ready) {
        return GPU_AMD_VRAM_ERR_INIT;
    }
    if (size_bytes == 0u || size_bytes > mgr->vram_size) {
        return GPU_AMD_VRAM_ERR_RANGE;
    }
    size = gpu_amd_vram_align_up(size_bytes, GPU_AMD_VRAM_BLOCK_SIZE);
    if (size == 0u || size < size_bytes) {
        return GPU_AMD_VRAM_ERR_RANGE;
    }

    for (i = 0; i < GPU_AMD_VRAM_MAX_ALLOCS; i++) {
        if (mgr->allocs[i].id == 0u) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return GPU_AMD_VRAM_ERR_NO_DESC;
    }

    for (i = 0; i < mgr->free_count; i++) {
        struct gpu_amd_vram_extent *e = &mgr->free_extents[i];
        if (e->size < size) {
            continue;
        }
        if (gpu_amd_vram_reserved_overlap(mgr, e->offset, size)) {
            continue;
        }
        mgr->allocs[slot].id = mgr->next_id++;
        if (mgr->next_id == 0u) {
            mgr->next_id = 1u;
        }
        mgr->allocs[slot].offset = e->offset;
        mgr->allocs[slot].size = size;
        mgr->allocs[slot].pattern = 0u;
        mgr->allocs[slot].filled = false;

        e->offset += size;
        e->size -= size;
        if (e->size == 0u) {
            for (int j = i; j < mgr->free_count - 1; j++) {
                mgr->free_extents[j] = mgr->free_extents[j + 1];
            }
            mgr->free_count--;
        }

        if (out_id != NULL) {
            *out_id = mgr->allocs[slot].id;
        }
        if (out_offset != NULL) {
            *out_offset = mgr->allocs[slot].offset;
        }
        return GPU_AMD_VRAM_OK;
    }

    return GPU_AMD_VRAM_ERR_NO_SPACE;
}

static inline int gpu_amd_vram_free(struct gpu_amd_vram_mgr *mgr, k_u32_t id)
{
    struct gpu_amd_vram_block *b;
    k_u64_t offset;
    k_u64_t size;
    int rc;

    if (mgr == NULL || !mgr->ready) {
        return GPU_AMD_VRAM_ERR_INIT;
    }
    if (id == 0u) {
        return GPU_AMD_VRAM_ERR_BAD_ID;
    }
    b = gpu_amd_vram_find(mgr, id);
    if (b == NULL) {
        return GPU_AMD_VRAM_ERR_BAD_ID;
    }
    if (gpu_amd_vram_reserved_overlap(mgr, b->offset, b->size)) {
        /* Should never happen for a valid allocation. */
        b->id = 0u;
        return GPU_AMD_VRAM_ERR_RESERVED;
    }

    offset = b->offset;
    size = b->size;
    b->id = 0u;
    b->offset = 0u;
    b->size = 0u;
    b->pattern = 0u;
    b->filled = false;

    rc = gpu_amd_vram_add_free_extent(mgr, offset, size);
    return rc;
}

/* Frame size in bytes from GRPH pitch (pixels) and visible height.
 * 4 bytes/pixel is the conservative assumption used by scanout_size. */
static inline k_u64_t gpu_amd_frame_bytes(k_u32_t pitch_pixels, k_u32_t height)
{
    k_u64_t p = (k_u64_t)(pitch_pixels & GPU_AMD_GRPH_PITCH__GRPH_PITCH_MASK);
    k_u64_t h = (k_u64_t)height;
    k_u64_t bytes;

    if (p == 0u || h == 0u) {
        return 0u;
    }
    bytes = p * h * 4ull;
    if (bytes / 4ull / p != h) {
        return 0u;
    }
    return bytes;
}

/* Find a VRAM offset for a frame-sized block outside the first 16 MiB and
 * outside all enabled scanout regions. Returns 0 when no safe place exists. */
static inline k_u64_t gpu_amd_choose_frame_offset(k_u64_t vram_bytes, k_u64_t frame_bytes,
                                                  const struct gpu_amd_scanout_region *regions,
                                                  int count)
{
    k_u64_t candidate;
    k_u64_t step = 4ull * 1024ull * 1024ull;
    int tries = 0;

    if (frame_bytes == 0u || frame_bytes > vram_bytes) {
        return 0u;
    }
    if (vram_bytes < GPU_AMD_VRAM_TEST_MIN_OFFSET + frame_bytes) {
        return 0u;
    }

    candidate = vram_bytes - frame_bytes;
    candidate &= ~((k_u64_t)4096u - 1ull);
    for (tries = 0; tries < 64 && candidate >= GPU_AMD_VRAM_TEST_MIN_OFFSET; tries++) {
        if (!gpu_amd_scanout_overlaps(candidate, frame_bytes, regions, count)) {
            return candidate;
        }
        if (candidate < step) {
            break;
        }
        candidate -= step;
    }

    return 0u;
}

#endif /* MICRO_OS_PORT_X86_64_GPU_AMD_H */
