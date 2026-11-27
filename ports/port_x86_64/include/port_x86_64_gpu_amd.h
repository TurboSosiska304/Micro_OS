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
