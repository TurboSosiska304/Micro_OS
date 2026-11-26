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

#endif /* MICRO_OS_PORT_X86_64_GPU_AMD_H */
