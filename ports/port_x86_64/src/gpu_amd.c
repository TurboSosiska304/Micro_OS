/*
 * Micro OS — the x86-64 port: AMD Polaris display controller probe.
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
 * GRPH_DEPTH/GRPH_FORMAT encoding and linear array-mode facts from
 * drivers/gpu/drm/amd/display/dc/dce110/dce110_mem_input_v.c and
 * drivers/gpu/drm/amd/display/dc/dc_hw_types.h (MIT, Linux v6.12).
 * Code written here.
 *
 * This is a read-only probe: it identifies the card from PCI config,
 * enables memory decode, records BAR layout, maps register MMIO, and reads
 * MC/GC configuration registers. It does not write display control registers,
 * does not load firmware, and does not start rings. The VRAM aperture probe
 * is read-only and does not write VRAM.
 */
#include "port_x86_64_gpu.h"
#include "port_x86_64_gpu_amd.h"
#include "port_x86_64_gpu_ids.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/fb.h"
#include "kernel/fb_ops.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/logfile.h"
#include "kshell/kshell.h"
#include "port_x86_64.h"
#include "port_x86_64_screen.h"

#define GPU_AMD_VRAM_PROBE_OFFSET (16ull * 1024ull * 1024ull)
#define GPU_AMD_VRAM_PROBE_BYTES 4096u

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

static k_u32_t amd_mmio_read32(k_u32_t mm_index)
{
    volatile k_u32_t *mmio = (volatile k_u32_t *)(uintptr_t)PORT_X86_64_GPU_VIRT;

    return mmio[mm_index];
}

static void amd_mmio_write32(k_u32_t mm_index, k_u32_t value)
{
    volatile k_u32_t *mmio = (volatile k_u32_t *)(uintptr_t)PORT_X86_64_GPU_VIRT;

    mmio[mm_index] = value;
}

static bool amd_mmio_mapped(k_u32_t mm_index, k_u32_t mmio_size)
{
    k_u64_t off = (k_u64_t)mm_index * 4ull;

    return off + 4ull <= (k_u64_t)mmio_size;
}

static void probe_vram_aperture(struct gpu_info *out, k_u64_t bar0_base, k_u64_t bar0_size)
{
    k_u64_t probe_phys;
    k_u32_t pages;
    volatile k_u32_t *p;
    k_u32_t i;
    k_u32_t sample = 0u;
    bool all_ff = true;
    bool all_00 = true;

    out->vram_read_ok = false;
    out->vram_sample = 0u;
    out->vram_probe_offset = 0u;

    if (bar0_base == 0u || bar0_size < GPU_AMD_VRAM_PROBE_OFFSET + GPU_AMD_VRAM_PROBE_BYTES) {
        return;
    }

    probe_phys = bar0_base + GPU_AMD_VRAM_PROBE_OFFSET;
    pages = port_x86_64_paging_map_mmio(PORT_X86_64_GPU_VRAM_VIRT, probe_phys,
                                        (k_size_t)GPU_AMD_VRAM_PROBE_BYTES);
    if (pages == 0u) {
        kwarn("gpu: BAR0 VRAM probe 0x%x could not be mapped", (unsigned)(k_u32_t)probe_phys);
        return;
    }

    p = (volatile k_u32_t *)(uintptr_t)PORT_X86_64_GPU_VRAM_VIRT;
    for (i = 0; i < (GPU_AMD_VRAM_PROBE_BYTES / 4u); i++) {
        k_u32_t v = p[i];

        if (i == 0u) {
            sample = v;
        }
        if (v != 0xFFFFFFFFu) {
            all_ff = false;
        }
        if (v != 0u) {
            all_00 = false;
        }
    }

    out->vram_read_ok = !(all_ff && sample == 0xFFFFFFFFu);
    out->vram_sample = sample;
    out->vram_probe_offset = GPU_AMD_VRAM_PROBE_OFFSET;

    if (all_ff) {
        kwarn("gpu: BAR0 VRAM probe at +%u MiB returned all 0xFF (mapping/bus suspect)",
              (unsigned)(GPU_AMD_VRAM_PROBE_OFFSET / (1024ull * 1024ull)));
        out->vram_read_ok = false;
    } else {
        kinfo("gpu: BAR0 VRAM probe at +%u MiB ok (sample 0x%08x%s)",
              (unsigned)(GPU_AMD_VRAM_PROBE_OFFSET / (1024ull * 1024ull)), (unsigned)sample,
              all_00 ? ", all-zero" : "");
    }
}

static void read_mc_regs(struct gpu_info *out)
{
    k_u32_t memsize;
    k_u32_t arb;
    k_u32_t chmap;
    k_u32_t l1tlb;
    k_u32_t seq_misc0;

    if (out->mmio_base == 0u || out->mmio_size == 0u) {
        return;
    }

    if (amd_mmio_mapped(GPU_AMD_MMCONFIG_MEMSIZE, out->mmio_size)) {
        memsize = amd_mmio_read32(GPU_AMD_MMCONFIG_MEMSIZE);
        out->mc_config_memsize = memsize;
        memsize = gpu_amd_decode_memsize_mb(memsize);
        out->vram_mc_bytes = (k_u64_t)memsize * 1024ull * 1024ull;
    }

    if (amd_mmio_mapped(GPU_AMD_MMMC_ARB_RAMCFG, out->mmio_size)) {
        arb = amd_mmio_read32(GPU_AMD_MMMC_ARB_RAMCFG);
        out->mc_arb_ramcfg = arb;
        out->mc_chansize = gpu_amd_decode_chansize(arb);
    }

    if (amd_mmio_mapped(GPU_AMD_MMMC_SHARED_CHMAP, out->mmio_size)) {
        chmap = amd_mmio_read32(GPU_AMD_MMMC_SHARED_CHMAP);
        out->mc_shared_chmap = chmap;
        out->mc_num_channels = gpu_amd_decode_noofchan(chmap);
    }

    if (amd_mmio_mapped(GPU_AMD_MMMC_VM_MX_L1_TLB_CNTL, out->mmio_size)) {
        l1tlb = amd_mmio_read32(GPU_AMD_MMMC_VM_MX_L1_TLB_CNTL);
        out->mc_vm_mx_l1_tlb_cntl = l1tlb;
    }

    if (amd_mmio_mapped(GPU_AMD_MMMC_SEQ_MISC0, out->mmio_size)) {
        seq_misc0 = amd_mmio_read32(GPU_AMD_MMMC_SEQ_MISC0);
        out->mc_seq_misc0 = seq_misc0;
    }

    if (amd_mmio_mapped(GPU_AMD_MMGB_ADDR_CONFIG, out->mmio_size)) {
        k_u32_t gb = amd_mmio_read32(GPU_AMD_MMGB_ADDR_CONFIG);

        if (gb != 0u && gb != 0xFFFFFFFFu) {
            out->gb_addr_config = gb;
            out->gc_num_se = (k_u8_t)(1u << gpu_amd_reg_field(
                                          gb, GPU_AMD_GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK,
                                          GPU_AMD_GB_ADDR_CONFIG__NUM_SHADER_ENGINES_SHIFT));
            /* gfx8/Polaris: 1 shader array per SE is the common topology. */
            out->gc_num_sh_per_se = 1u;
        }
    }
}

static void fill_identity(struct gpu_info *out, const struct port_x86_64_pci_device *pci,
                          k_u16_t vendor, k_u16_t device, k_u8_t revision, const char *name)
{
    k_u64_t bar0_base;
    k_u64_t bar5_base;
    k_u32_t bar5_size;
    k_u64_t out_vram = 0;

    /* Enable memory decode so BAR MMIO can be read/mapped. This is PCI config,
     * not a display engine register write. Bus mastering is not needed for
     * read-only identification. */
    k_u16_t command = port_x86_64_pci_read16(pci, PCI_COMMAND);
    if ((command & PCI_COMMAND_MEMORY) == 0u) {
        port_x86_64_pci_enable(pci, PCI_COMMAND_MEMORY);
    }

    bar0_base = bar_base(pci, PCI_BAR0);
    if (bar_is_mem64(pci, PCI_BAR0)) {
        out_vram = bar_size64(pci, PCI_BAR0);
    } else {
        out_vram = (k_u64_t)port_x86_64_pci_bar_size(pci, PCI_BAR0);
    }

    bar5_base = bar_base(pci, 0x24u); /* BAR5 */
    bar5_size = port_x86_64_pci_bar_size(pci, 0x24u);

    out->present = true;
    out->vendor = vendor;
    out->device = device;
    out->revision = revision;
    out->bus = pci->bus;
    out->slot = pci->slot;
    out->function = pci->function;
    out->mmio_base = bar5_base;
    out->mmio_size = bar5_size;
    if (bar0_base != 0u) {
        out->vram_bytes = out_vram;
    }

    if (name != NULL) {
        k_size_t n = 0;
        while (name[n] != '\0' && n + 1u < sizeof(out->name)) {
            out->name[n] = name[n];
            n++;
        }
        out->name[n] = '\0';
    }

    /* Map register MMIO BAR (BAR5) as uncached MMIO. */
    if (out->mmio_base != 0u && out->mmio_size != 0u) {
        k_u32_t pages = port_x86_64_paging_map_mmio(PORT_X86_64_GPU_VIRT, out->mmio_base,
                                                    (k_size_t)out->mmio_size);
        if (pages == 0u) {
            kwarn("gpu: BAR5 MMIO 0x%x size 0x%x could not be mapped",
                  (unsigned)(k_u32_t)out->mmio_base, (unsigned)out->mmio_size);
            out->mmio_base = 0u;
            out->mmio_size = 0u;
        } else {
            kinfo("gpu: BAR5 MMIO mapped at virt 0x%x (phys 0x%x, %u KiB)",
                  (unsigned)(k_u32_t)PORT_X86_64_GPU_VIRT, (unsigned)(k_u32_t)out->mmio_base,
                  (unsigned)(out->mmio_size / 1024u));
            read_mc_regs(out);
        }
    }

    /* Read-only VRAM aperture probe: no writes to VRAM. */
    probe_vram_aperture(out, bar0_base, out_vram);
}

enum gpu_probe_result port_x86_64_gpu_amd_probe(const struct port_x86_64_pci_device *pci,
                                                struct gpu_info *out)
{
    k_u16_t vendor;
    k_u16_t device;
    k_u8_t revision;
    const char *name;

    if (pci == NULL || out == NULL) {
        return GPU_PROBE_NONE;
    }

    vendor = port_x86_64_pci_read16(pci, PCI_VENDOR);
    device = port_x86_64_pci_read16(pci, PCI_DEVICE);
    revision = (k_u8_t)(port_x86_64_pci_read16(pci, PCI_REVISION) & 0xFFu);

    if (vendor != 0x1002u) {
        return GPU_PROBE_NONE;
    }

    name = port_x86_64_gpu_amd_lookup(vendor, device);
    if (name == NULL) {
        fill_identity(out, pci, vendor, device, revision, "AMD display controller");
        out->known = false;
        return GPU_PROBE_UNKNOWN_DEV;
    }

    fill_identity(out, pci, vendor, device, revision, name);
    out->known = true;
    return GPU_PROBE_OK;
}

static int read_scanout_regions(struct gpu_amd_scanout_region *out, int max_out)
{
    int n = 0;
    k_u8_t pipe;
    k_u32_t tiling_control = 0u;
    bool tiling_known = false;
    k_u64_t bar0_base = 0u;
    k_u64_t vram_size = port_x86_64_gpu_info() != NULL ? port_x86_64_gpu_info()->vram_bytes : 0u;

    if (out == NULL || max_out <= 0) {
        return 0;
    }
    if (port_x86_64_gpu_info() != NULL) {
        struct port_x86_64_pci_device pci;

        pci.bus = port_x86_64_gpu_info()->bus;
        pci.slot = port_x86_64_gpu_info()->slot;
        pci.function = port_x86_64_gpu_info()->function;
        bar0_base = bar_base(&pci, PCI_BAR0);
    }
    if (amd_mmio_mapped(GPU_AMD_MMDCP_UNP_GRPH_CONTROL,
                        (k_u32_t)port_x86_64_gpu_info()->mmio_size)) {
        tiling_control = amd_mmio_read32(GPU_AMD_MMDCP_UNP_GRPH_CONTROL);
        tiling_known = true;
    }

    for (pipe = 0; pipe < GPU_AMD_DCE_PIPE_COUNT && n < max_out; pipe++) {
        k_u32_t off = gpu_amd_dcp_offset(pipe);
        k_u32_t enable_reg = GPU_AMD_MMDCP_GRPH_ENABLE + off;
        k_u32_t enable;
        k_u32_t pitch;
        k_u32_t y_end;
        k_u32_t control;
        struct gpu_amd_plane_format_info format_info;
        k_u32_t lo;
        k_u32_t hi;
        k_u64_t addr;
        k_u64_t offset;
        k_u64_t size;

        if (!amd_mmio_mapped(enable_reg, (k_u32_t)port_x86_64_gpu_info()->mmio_size)) {
            continue;
        }

        enable = amd_mmio_read32(enable_reg);
        if ((enable & GPU_AMD_GRPH_ENABLE__GRPH_ENABLE_MASK) == 0u) {
            continue;
        }

        if (!amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + off,
                             (k_u32_t)port_x86_64_gpu_info()->mmio_size) ||
            !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off,
                             (k_u32_t)port_x86_64_gpu_info()->mmio_size) ||
            !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_CONTROL + off,
                             (k_u32_t)port_x86_64_gpu_info()->mmio_size) ||
            !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_PITCH + off,
                             (k_u32_t)port_x86_64_gpu_info()->mmio_size) ||
            !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_Y_END + off,
                             (k_u32_t)port_x86_64_gpu_info()->mmio_size)) {
            continue;
        }

        lo = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + off);
        hi = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off);
        control = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_CONTROL + off);
        pitch = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PITCH + off);
        y_end = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_Y_END + off);
        addr = ((k_u64_t)hi << 32) | (k_u64_t)lo;
        format_info = gpu_amd_plane_decode(control);
        size = gpu_amd_scanout_size(pitch, y_end, format_info.bytes_per_pixel);

        out[n].pipe = pipe;
        out[n].primary = true;
        out[n].enabled = true;
        out[n].register_address = addr;
        out[n].address = addr;
        out[n].address_valid = gpu_amd_scanout_offset(addr, bar0_base, vram_size, &offset);
        if (out[n].address_valid) {
            out[n].address = offset;
        }
        out[n].pitch_pixels = pitch & GPU_AMD_GRPH_PITCH__GRPH_PITCH_MASK;
        out[n].height = (y_end & GPU_AMD_GRPH_Y_END__GRPH_Y_END_MASK) + 1u;
        out[n].size_bytes = size;
        out[n].control = control;
        out[n].tiling_control = tiling_control;
        out[n].linear = tiling_known && gpu_amd_plane_is_linear(tiling_control);
        out[n].format = format_info.format;
        out[n].bytes_per_pixel = format_info.bytes_per_pixel;
        n++;
        if (n >= max_out) {
            break;
        }

        if (amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_SECONDARY_SURFACE_ADDRESS + off,
                            (k_u32_t)port_x86_64_gpu_info()->mmio_size) &&
            amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_SECONDARY_SURFACE_ADDRESS_HIGH + off,
                            (k_u32_t)port_x86_64_gpu_info()->mmio_size)) {
            k_u32_t slo = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_SECONDARY_SURFACE_ADDRESS + off);
            k_u32_t shi = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_SECONDARY_SURFACE_ADDRESS_HIGH + off);
            k_u64_t saddr = ((k_u64_t)shi << 32) | (k_u64_t)slo;
            k_u64_t soffset;
            if (saddr != 0u && saddr != addr) {
                out[n].pipe = pipe;
                out[n].primary = false;
                out[n].enabled = true;
                out[n].register_address = saddr;
                out[n].address = saddr;
                out[n].address_valid =
                    gpu_amd_scanout_offset(saddr, bar0_base, vram_size, &soffset);
                if (out[n].address_valid) {
                    out[n].address = soffset;
                }
                out[n].pitch_pixels = out[n - 1].pitch_pixels;
                out[n].height = out[n - 1].height;
                out[n].size_bytes = size;
                out[n].control = control;
                out[n].tiling_control = tiling_control;
                out[n].linear = tiling_known && gpu_amd_plane_is_linear(tiling_control);
                out[n].format = format_info.format;
                out[n].bytes_per_pixel = format_info.bytes_per_pixel;
                n++;
            }
        }
    }

    return n;
}

static int do_vram_write_test(k_u64_t bar0_base, k_u64_t test_offset)
{
    k_u32_t pages;
    volatile k_u32_t *p;
    k_u32_t saved[GPU_AMD_VRAM_TEST_BYTES / 4u];
    k_u32_t i;
    k_u32_t bad_index = 0u;
    k_u32_t bad_got = 0u;
    k_u32_t bad_expect = 0u;
    bool ok = true;
    bool restored = true;

    pages = port_x86_64_paging_map_mmio(PORT_X86_64_GPU_VRAM_VIRT, bar0_base + test_offset,
                                        (k_size_t)GPU_AMD_VRAM_TEST_BYTES);
    if (pages == 0u) {
        kshell_printf("gpu vramtest: FAIL map BAR0+0x%x\n", (unsigned)test_offset);
        kerror("gpu vramtest: map failed at 0x%x", (unsigned)test_offset);
        return 1;
    }

    p = (volatile k_u32_t *)(uintptr_t)PORT_X86_64_GPU_VRAM_VIRT;

    /* Read original contents before touching anything. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        saved[i] = p[i];
    }

    /* The marker goes to the log before the first store, so a machine that
     * hangs on a write still says what it was doing. */
    kinfo("gpu vramtest: writing BAR0+0x%x, %u bytes", (unsigned)test_offset,
          (unsigned)GPU_AMD_VRAM_TEST_BYTES);
    k_logfile_flush();

    /* Write pattern: 0xA5000000 ^ index. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        p[i] = 0xA5000000u ^ i;
    }

    /* Read back and verify; keep the first failure for the result line. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        k_u32_t expect = 0xA5000000u ^ i;
        k_u32_t got = p[i];

        if (got != expect) {
            if (ok) {
                bad_index = i;
                bad_got = got;
                bad_expect = expect;
            }
            kerror("gpu vramtest: mismatch at +%u got 0x%08x expect 0x%08x", (unsigned)(i * 4u),
                   (unsigned)got, (unsigned)expect);
            ok = false;
        }
    }

    /* Restore original contents even when the pattern did not stick. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        p[i] = saved[i];
    }
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        if (p[i] != saved[i]) {
            kerror("gpu vramtest: restore mismatch at +%u", (unsigned)(i * 4u));
            restored = false;
            break;
        }
    }

    if (!ok) {
        kshell_printf("gpu vramtest: FAIL addr=BAR0+0x%x offset=+%u expected=0x%08x"
                      " got=0x%08x restore=%s\n",
                      (unsigned)test_offset, (unsigned)(bad_index * 4u), (unsigned)bad_expect,
                      (unsigned)bad_got, restored ? "ok" : "FAILED");
        return 1;
    }
    if (!restored) {
        kshell_printf("gpu vramtest: FAIL addr=BAR0+0x%x write/read ok, restore FAILED\n",
                      (unsigned)test_offset);
        kerror("gpu vramtest: restore failed after a good write/read");
        return 1;
    }

    kinfo("gpu vramtest: write/read ok at 0x%x, restored", (unsigned)test_offset);
    kshell_printf("gpu vramtest: PASS addr=BAR0+0x%x size=%u write/read ok, original restored\n",
                  (unsigned)test_offset, (unsigned)GPU_AMD_VRAM_TEST_BYTES);
    return 0;
}

int port_x86_64_gpu_amd_vramtest(bool do_write, bool confirmed)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    struct gpu_amd_scanout_region regions[12];
    int region_count = 0;
    k_u64_t bar0_base = 0u;
    k_u64_t test_offset;
    int i;

    if (info == NULL || !info->present) {
        kshell_printf("gpu vramtest: no display controller present\n");
        return 1;
    }
    if (info->vendor != 0x1002u) {
        kshell_printf("gpu vramtest: not an AMD controller (%04x:%04x)\n", (unsigned)info->vendor,
                      (unsigned)info->device);
        return 1;
    }
    if (info->mmio_base == 0u || info->mmio_size == 0u) {
        kshell_printf("gpu vramtest: BAR5 MMIO not mapped\n");
        return 1;
    }

    /* BAR0 base: we don't store it separately; recover from PCI. */
    {
        struct port_x86_64_pci_device pci;
        pci.bus = info->bus;
        pci.slot = info->slot;
        pci.function = info->function;
        bar0_base = bar_base(&pci, PCI_BAR0);
    }

    if (bar0_base == 0u || info->vram_bytes == 0u) {
        kshell_printf("gpu vramtest: BAR0 aperture unknown\n");
        return 1;
    }

    region_count = read_scanout_regions(regions, (int)(sizeof(regions) / sizeof(regions[0])));
    for (i = 0; i < region_count; i++) {
        if (regions[i].enabled && !regions[i].address_valid) {
            kshell_printf("gpu vramtest: REFUSED, scanout address is outside VRAM/BAR0\n");
            return 1;
        }
    }

    kshell_printf("gpu vramtest: scanout regions: %d\n", region_count);
    for (i = 0; i < region_count; i++) {
        kshell_printf("  pipe %u %s: addr 0x%x%08x off 0x%x%08x size %u KiB (%ux%u)\n",
                      (unsigned)regions[i].pipe, regions[i].primary ? "primary" : "secondary",
                      (unsigned)(regions[i].register_address >> 32),
                      (unsigned)(k_u32_t)regions[i].register_address,
                      (unsigned)(regions[i].address >> 32), (unsigned)(k_u32_t)regions[i].address,
                      (unsigned)(regions[i].size_bytes / 1024u), (unsigned)regions[i].pitch_pixels,
                      (unsigned)regions[i].height);
        kinfo("gpu vramtest: pipe %u %s addr 0x%x%08x off 0x%x%08x size %u",
              (unsigned)regions[i].pipe, regions[i].primary ? "primary" : "secondary",
              (unsigned)(regions[i].register_address >> 32),
              (unsigned)(k_u32_t)regions[i].register_address, (unsigned)(regions[i].address >> 32),
              (unsigned)(k_u32_t)regions[i].address, (unsigned)regions[i].size_bytes);
    }
    if (region_count == 0) {
        kshell_printf("  (no enabled GRPH scanout detected)\n");
    }

    test_offset = gpu_amd_choose_test_offset(info->vram_bytes, regions, region_count);
    if (test_offset == 0u) {
        kshell_printf("gpu vramtest: no safe region found outside scanout/first 16 MiB\n");
        return 1;
    }

    kshell_printf("  candidate: BAR0+0x%x (last-4MiB area, overlap checked)\n",
                  (unsigned)test_offset);

    if (!do_write) {
        kshell_printf("gpu vramtest: read-only check complete; pass --write --yes to write\n");
        return 0;
    }
    if (!confirmed) {
        kshell_printf("gpu vramtest: dry-run, candidate BAR0+0x%x; pass --yes to write\n",
                      (unsigned)test_offset);
        return 0;
    }

    if (region_count == 0) {
        kshell_printf("gpu vramtest: REFUSED, scanout not found; cannot prove safe write\n");
        kerror("gpu vramtest: refused, no scanout found");
        return 1;
    }

    if (gpu_amd_scanout_overlaps(test_offset, GPU_AMD_VRAM_TEST_BYTES, regions, region_count)) {
        kshell_printf("gpu vramtest: REFUSED, candidate overlaps scanout\n");
        kerror("gpu vramtest: refused overlap");
        return 1;
    }
    if (test_offset < GPU_AMD_VRAM_TEST_MIN_OFFSET) {
        kshell_printf("gpu vramtest: REFUSED, candidate inside first 16 MiB\n");
        return 1;
    }

    return do_vram_write_test(bar0_base, test_offset);
}

/* ------------------------------------------------------------------------- */
/* VRAM block manager: vrammap / vramalloc / vramfill / vramread / vramfree  */
/* / vramdump. Original work for Micro OS.                                    */
/* ------------------------------------------------------------------------- */

static struct gpu_amd_vram_mgr g_vram_mgr;
static volatile k_u8_t *amd_map_vram(k_u64_t bar0_base, k_u64_t offset, k_size_t bytes);

struct gpu_amd_flip_state {
    bool active;
    k_u32_t id;
    k_u8_t pipe;
    k_u64_t original_address;
    k_u64_t active_offset;
    k_u32_t original_pitch;
    k_u32_t original_control;
    k_u32_t original_tiling;
};

static struct gpu_amd_flip_state s_amd_flip;
static struct gpu_amd_surface_slot s_amd_surface;
static bool s_amd_surface_armed;

static bool gpu_amd_get_bar0(k_u64_t *out_base, k_u64_t *out_size)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    struct port_x86_64_pci_device pci;

    if (out_base != NULL) {
        *out_base = 0u;
    }
    if (out_size != NULL) {
        *out_size = 0u;
    }
    if (info == NULL || !info->present || info->vendor != 0x1002u) {
        return false;
    }

    pci.bus = info->bus;
    pci.slot = info->slot;
    pci.function = info->function;
    if (out_base != NULL) {
        *out_base = bar_base(&pci, PCI_BAR0);
    }
    if (out_size != NULL) {
        *out_size = info->vram_bytes;
    }
    return out_base == NULL || *out_base != 0u;
}

static int gpu_amd_vram_ensure_init(void)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    struct gpu_amd_scanout_region regions[GPU_AMD_DCE_PIPE_COUNT * 2];
    int count = 0;
    int rc;

    if (info == NULL || !info->present) {
        return GPU_AMD_VRAM_ERR_INIT;
    }
    if (info->vendor != 0x1002u) {
        return GPU_AMD_VRAM_ERR_INIT;
    }
    if (info->mmio_base == 0u || info->mmio_size == 0u) {
        return GPU_AMD_VRAM_ERR_NOT_MAPPED;
    }
    if (info->vram_bytes == 0u) {
        return GPU_AMD_VRAM_ERR_INIT;
    }

    count = read_scanout_regions(regions, (int)(sizeof(regions) / sizeof(regions[0])));
    for (int i = 0; i < count; i++) {
        if (regions[i].enabled && !regions[i].address_valid) {
            return GPU_AMD_VRAM_ERR_RANGE;
        }
    }
    if (g_vram_mgr.ready && g_vram_mgr.vram_size == info->vram_bytes) {
        return GPU_AMD_VRAM_OK;
    }

    rc = gpu_amd_vram_mgr_init(&g_vram_mgr, info->vram_bytes, regions, count);
    if (rc != GPU_AMD_VRAM_OK) {
        return rc;
    }
    return GPU_AMD_VRAM_OK;
}

int port_x86_64_gpu_amd_vrammap(void)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    struct gpu_amd_scanout_region regions[GPU_AMD_DCE_PIPE_COUNT * 2];
    int count;
    int i;
    k_u64_t free_total = 0u;
    int rc;

    if (info == NULL || !info->present) {
        kshell_printf("gpu vrammap: no display controller present\n");
        return 1;
    }
    if (info->vendor != 0x1002u) {
        kshell_printf("gpu vrammap: not an AMD controller (%04x:%04x)\n", (unsigned)info->vendor,
                      (unsigned)info->device);
        return 1;
    }
    if (info->mmio_base == 0u || info->mmio_size == 0u) {
        kshell_printf("gpu vrammap: BAR5 MMIO not mapped\n");
        return 1;
    }

    count = read_scanout_regions(regions, (int)(sizeof(regions) / sizeof(regions[0])));
    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu vrammap: init failed: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }

    kshell_printf("gpu vrammap: vram %u MiB, block %u KiB\n",
                  (unsigned)(info->vram_bytes / (1024ull * 1024ull)),
                  (unsigned)(GPU_AMD_VRAM_BLOCK_SIZE / 1024ull));
    kshell_printf("  reserved: [0x00000000, 0x%08x) first 16 MiB\n",
                  (unsigned)GPU_AMD_VRAM_RESERVED_END);
    if (count == 0) {
        kshell_printf("  scanout: none detected\n");
    }
    for (i = 0; i < count; i++) {
        kshell_printf("  scanout: pipe %u %s [0x%x%08x, +%u KiB) off=0x%x%08x pitch=%u height=%u "
                      "format=%s bpp=%u control=0x%08x tiling=0x%08x linear=%s\n",
                      (unsigned)regions[i].pipe, regions[i].primary ? "primary" : "secondary",
                      (unsigned)(regions[i].register_address >> 32),
                      (unsigned)(k_u32_t)regions[i].register_address,
                      (unsigned)(regions[i].size_bytes / 1024u),
                      (unsigned)(regions[i].address >> 32), (unsigned)(k_u32_t)regions[i].address,
                      (unsigned)regions[i].pitch_pixels, (unsigned)regions[i].height,
                      gpu_amd_plane_format_name(regions[i].format),
                      (unsigned)regions[i].bytes_per_pixel, (unsigned)regions[i].control,
                      (unsigned)regions[i].tiling_control, regions[i].linear ? "yes" : "no");
    }
    for (i = 0; i < g_vram_mgr.free_count; i++) {
        free_total += g_vram_mgr.free_extents[i].size;
        kshell_printf("  free: [0x%x%08x, +%u MiB)\n",
                      (unsigned)(g_vram_mgr.free_extents[i].offset >> 32),
                      (unsigned)(k_u32_t)g_vram_mgr.free_extents[i].offset,
                      (unsigned)(g_vram_mgr.free_extents[i].size / (1024ull * 1024ull)));
    }
    kshell_printf("  free total: %u MiB in %d extent(s)\n",
                  (unsigned)(free_total / (1024ull * 1024ull)), g_vram_mgr.free_count);
    kinfo("gpu vrammap: vram=%uMiB free=%uMiB extents=%d scanouts=%d",
          (unsigned)(info->vram_bytes / (1024ull * 1024ull)),
          (unsigned)(free_total / (1024ull * 1024ull)), g_vram_mgr.free_count, count);
    return 0;
}

int port_x86_64_gpu_amd_vramalloc(k_u64_t size_kib)
{
    k_u32_t id = 0u;
    k_u64_t offset = 0u;
    k_u64_t bytes;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu vramalloc: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    if (size_kib == 0u || size_kib > (k_u64_t)(~(k_u32_t)0u)) {
        kshell_printf("gpu vramalloc: invalid size\n");
        return 1;
    }
    bytes = size_kib * 1024ull;
    rc = gpu_amd_vram_alloc(&g_vram_mgr, bytes, &id, &offset);
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu vramalloc: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    kshell_printf("gpu vramalloc: id=%u offset=0x%x%08x size=%u KiB\n", (unsigned)id,
                  (unsigned)(offset >> 32), (unsigned)(k_u32_t)offset,
                  (unsigned)(gpu_amd_vram_find(&g_vram_mgr, id)->size / 1024ull));
    kinfo("gpu vramalloc: id=%u offset=0x%x%08x", (unsigned)id, (unsigned)(offset >> 32),
          (unsigned)(k_u32_t)offset);
    return 0;
}

static k_u32_t gpu_amd_vram_pattern(k_u32_t pattern, k_u64_t block_offset, k_u64_t word_index)
{
    /* Deterministic pattern by block offset so reads can verify content. */
    return pattern ^ (k_u32_t)(block_offset >> 12) ^ (k_u32_t)word_index;
}

int port_x86_64_gpu_amd_vramfill(k_u32_t id, k_u32_t pattern, bool confirmed)
{
    struct gpu_amd_vram_block *b;
    k_u64_t bar0_base = 0u;
    k_u64_t bar0_size = 0u;
    k_u64_t off;
    volatile k_u8_t *mapped;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu vramfill: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    b = gpu_amd_vram_find(&g_vram_mgr, id);
    if (b == NULL) {
        kshell_printf("gpu vramfill: bad id %u\n", (unsigned)id);
        return 1;
    }
    if (gpu_amd_vram_reserved_overlap(&g_vram_mgr, b->offset, b->size)) {
        kshell_printf("gpu vramfill: refused, block overlaps reserved/scanout\n");
        return 1;
    }
    if (!gpu_amd_get_bar0(&bar0_base, &bar0_size) || bar0_base == 0u) {
        kshell_printf("gpu vramfill: BAR0 not available\n");
        return 1;
    }
    if (b->offset > bar0_size || b->size > bar0_size - b->offset) {
        kshell_printf("gpu vramfill: block outside BAR0 aperture\n");
        return 1;
    }
    if (!confirmed) {
        kshell_printf("gpu vramfill: dry-run id=%u pattern=0x%08x; pass --yes to write\n",
                      (unsigned)id, (unsigned)pattern);
        return 0;
    }

    /* The marker precedes the stores so a hang is distinguishable from a slow
     * fill in the log. */
    kinfo("gpu vramfill: writing id=%u pattern=0x%08x", (unsigned)id, (unsigned)pattern);
    k_logfile_flush();
    for (off = 0; off < b->size; off += GPU_AMD_VRAM_BLOCK_SIZE) {
        volatile k_u32_t *p;
        k_u64_t chunk = b->size - off;
        k_u32_t i;
        k_u32_t words;

        if (chunk > GPU_AMD_VRAM_BLOCK_SIZE) {
            chunk = GPU_AMD_VRAM_BLOCK_SIZE;
        }
        mapped = amd_map_vram(bar0_base, b->offset + off, (k_size_t)chunk);
        if (mapped == NULL) {
            kshell_printf("gpu vramfill: map failed at +0x%x\n", (unsigned)(b->offset + off));
            return 1;
        }
        p = (volatile k_u32_t *)(uintptr_t)mapped;
        words = (k_u32_t)(chunk / 4u);
        for (i = 0; i < words; i++) {
            p[i] = gpu_amd_vram_pattern(pattern, b->offset, (off / 4u) + (k_u64_t)i);
        }
    }

    /* The aperture is write-combining: drain the stores before a later
     * readback or a scanout flip can observe them. */
    __asm__ volatile("mfence" : : : "memory");
    b->pattern = pattern;
    b->filled = true;
    kshell_printf("gpu vramfill: id=%u pattern=0x%08x size=%u KiB OK\n", (unsigned)id,
                  (unsigned)pattern, (unsigned)(b->size / 1024ull));
    kinfo("gpu vramfill: id=%u pattern=0x%08x", (unsigned)id, (unsigned)pattern);
    return 0;
}

int port_x86_64_gpu_amd_vramread(k_u32_t id)
{
    struct gpu_amd_vram_block *b;
    k_u64_t bar0_base = 0u;
    k_u64_t bar0_size = 0u;
    k_u64_t off;
    volatile k_u8_t *mapped;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu vramread: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    b = gpu_amd_vram_find(&g_vram_mgr, id);
    if (b == NULL) {
        kshell_printf("gpu vramread: bad id %u\n", (unsigned)id);
        return 1;
    }
    if (!b->filled) {
        kshell_printf("gpu vramread: id=%u not filled\n", (unsigned)id);
        return 1;
    }
    if (!gpu_amd_get_bar0(&bar0_base, &bar0_size) || bar0_base == 0u) {
        kshell_printf("gpu vramread: BAR0 not available\n");
        return 1;
    }
    if (b->offset > bar0_size || b->size > bar0_size - b->offset) {
        kshell_printf("gpu vramread: block outside BAR0 aperture\n");
        return 1;
    }

    for (off = 0; off < b->size; off += GPU_AMD_VRAM_BLOCK_SIZE) {
        volatile k_u32_t *p;
        k_u64_t chunk = b->size - off;
        k_u32_t i;
        k_u32_t words;

        if (chunk > GPU_AMD_VRAM_BLOCK_SIZE) {
            chunk = GPU_AMD_VRAM_BLOCK_SIZE;
        }
        mapped = amd_map_vram(bar0_base, b->offset + off, (k_size_t)chunk);
        if (mapped == NULL) {
            kshell_printf("gpu vramread: map failed at +0x%x\n", (unsigned)(b->offset + off));
            return 1;
        }
        p = (volatile k_u32_t *)(uintptr_t)mapped;
        words = (k_u32_t)(chunk / 4u);
        for (i = 0; i < words; i++) {
            k_u32_t expect = gpu_amd_vram_pattern(b->pattern, b->offset, (off / 4u) + (k_u64_t)i);
            k_u32_t got = p[i];
            if (got != expect) {
                kshell_printf("gpu vramread: mismatch id=%u at +%u: got 0x%08x expect 0x%08x\n",
                              (unsigned)id, (unsigned)((off + (k_u64_t)i * 4u)), (unsigned)got,
                              (unsigned)expect);
                kerror("gpu vramread: mismatch id=%u", (unsigned)id);
                return 1;
            }
        }
    }

    kshell_printf("gpu vramread: id=%u pattern=0x%08x size=%u KiB OK\n", (unsigned)id,
                  (unsigned)b->pattern, (unsigned)(b->size / 1024ull));
    kinfo("gpu vramread: id=%u ok", (unsigned)id);
    return 0;
}

int port_x86_64_gpu_amd_vramfree(k_u32_t id)
{
    struct gpu_amd_vram_block *buffer;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu vramfree: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    buffer = gpu_amd_vram_find(&g_vram_mgr, id);
    if (buffer == NULL) {
        kshell_printf("gpu vramfree: bad id %u\n", (unsigned)id);
        return 1;
    }
    if ((s_amd_surface.mapped && s_amd_surface.id == id) ||
        (s_amd_flip.active && s_amd_flip.id == id)) {
        kshell_printf("gpu vramfree: id=%u is mapped or active; unmap/restore first\n",
                      (unsigned)id);
        return 1;
    }
    rc = gpu_amd_vram_free(&g_vram_mgr, id);
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu vramfree: id=%u: %s\n", (unsigned)id, gpu_amd_vram_err_name(rc));
        return 1;
    }
    kshell_printf("gpu vramfree: id=%u freed\n", (unsigned)id);
    kinfo("gpu vramfree: id=%u", (unsigned)id);
    return 0;
}

int port_x86_64_gpu_amd_vramdump(void)
{
    int i;
    int used = 0;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu vramdump: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }

    kshell_printf("gpu vramdump: %u MiB total, %u KiB blocks, %d free extents\n",
                  (unsigned)(g_vram_mgr.vram_size / (1024ull * 1024ull)),
                  (unsigned)(GPU_AMD_VRAM_BLOCK_SIZE / 1024ull), g_vram_mgr.free_count);
    for (i = 0; i < GPU_AMD_VRAM_MAX_ALLOCS; i++) {
        const struct gpu_amd_vram_block *b = &g_vram_mgr.allocs[i];
        if (b->id == 0u) {
            continue;
        }
        used++;
        kshell_printf("  id=%u off=0x%x%08x size=%u KiB %s", (unsigned)b->id,
                      (unsigned)(b->offset >> 32), (unsigned)(k_u32_t)b->offset,
                      (unsigned)(b->size / 1024ull), b->filled ? "filled" : "empty");
        if (b->filled) {
            kshell_printf(" pattern=0x%08x", (unsigned)b->pattern);
        }
        if (b->name[0] != '\0') {
            kshell_printf(" name=%s", b->name);
        }
        kshell_printf("\n");
    }
    if (used == 0) {
        kshell_printf("  (no allocations)\n");
    }
    return 0;
}

static k_u32_t gpu_amd_buf_default_pattern(k_u32_t id)
{
    return 0xB0F00000u ^ id;
}

int port_x86_64_gpu_amd_bufalloc(k_u64_t size_kib, const char *name)
{
    k_u32_t id = 0u;
    k_u64_t offset = 0u;
    k_u64_t bytes;
    int rc;
    const char *use_name = (name != NULL && name[0] != '\0') ? name : "buf";

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu bufalloc: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    if (size_kib == 0u || size_kib > (k_u64_t)(~(k_u32_t)0u)) {
        kshell_printf("gpu bufalloc: invalid size\n");
        return 1;
    }
    bytes = size_kib * 1024ull;
    rc = gpu_amd_vram_alloc_named(&g_vram_mgr, bytes, &id, &offset, use_name);
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu bufalloc: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    {
        struct gpu_amd_vram_block *b = gpu_amd_vram_find(&g_vram_mgr, id);
        kshell_printf("gpu bufalloc: id=%u name=%s offset=0x%x%08x size=%u KiB\n", (unsigned)id,
                      b != NULL ? b->name : use_name, (unsigned)(offset >> 32),
                      (unsigned)(k_u32_t)offset,
                      (unsigned)((b != NULL ? b->size : bytes) / 1024ull));
    }
    kinfo("gpu bufalloc: id=%u name=%s offset=0x%x%08x", (unsigned)id, use_name,
          (unsigned)(offset >> 32), (unsigned)(k_u32_t)offset);
    return 0;
}

int port_x86_64_gpu_amd_buffill(k_u32_t id, bool confirmed)
{
    struct gpu_amd_vram_block *b;
    k_u64_t bar0_base = 0u;
    k_u64_t bar0_size = 0u;
    k_u64_t off;
    volatile k_u8_t *mapped;
    k_u32_t pattern;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu buffill: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    b = gpu_amd_vram_find(&g_vram_mgr, id);
    if (b == NULL) {
        kshell_printf("gpu buffill: bad id %u\n", (unsigned)id);
        return 1;
    }
    if (gpu_amd_vram_reserved_overlap(&g_vram_mgr, b->offset, b->size)) {
        kshell_printf("gpu buffill: refused, block overlaps reserved/scanout\n");
        return 1;
    }
    if (!gpu_amd_get_bar0(&bar0_base, &bar0_size) || bar0_base == 0u) {
        kshell_printf("gpu buffill: BAR0 not available\n");
        return 1;
    }
    if (b->offset > bar0_size || b->size > bar0_size - b->offset) {
        kshell_printf("gpu buffill: block outside BAR0 aperture\n");
        return 1;
    }

    pattern = gpu_amd_buf_default_pattern(id);
    if (!confirmed) {
        kshell_printf("gpu buffill: dry-run id=%u pattern=0x%08x; pass --yes to write\n",
                      (unsigned)id, (unsigned)pattern);
        return 0;
    }
    kinfo("gpu buffill: writing id=%u pattern=0x%08x", (unsigned)id, (unsigned)pattern);
    k_logfile_flush();
    for (off = 0; off < b->size; off += GPU_AMD_VRAM_BLOCK_SIZE) {
        volatile k_u32_t *p;
        k_u64_t chunk = b->size - off;
        k_u32_t i;
        k_u32_t words;

        if (chunk > GPU_AMD_VRAM_BLOCK_SIZE) {
            chunk = GPU_AMD_VRAM_BLOCK_SIZE;
        }
        mapped = amd_map_vram(bar0_base, b->offset + off, (k_size_t)chunk);
        if (mapped == NULL) {
            kshell_printf("gpu buffill: map failed at +0x%x\n", (unsigned)(b->offset + off));
            return 1;
        }
        p = (volatile k_u32_t *)(uintptr_t)mapped;
        words = (k_u32_t)(chunk / 4u);
        for (i = 0; i < words; i++) {
            p[i] = gpu_amd_vram_pattern(pattern, b->offset, (off / 4u) + (k_u64_t)i);
        }
    }

    __asm__ volatile("mfence" : : : "memory");
    b->pattern = pattern;
    b->filled = true;
    kshell_printf("gpu buffill: id=%u name=%s pattern=0x%08x size=%u KiB OK\n", (unsigned)id,
                  b->name[0] != '\0' ? b->name : "-", (unsigned)pattern,
                  (unsigned)(b->size / 1024ull));
    kinfo("gpu buffill: id=%u pattern=0x%08x", (unsigned)id, (unsigned)pattern);
    return 0;
}

int port_x86_64_gpu_amd_bufread(k_u32_t id)
{
    struct gpu_amd_vram_block *b;
    k_u64_t bar0_base = 0u;
    k_u64_t bar0_size = 0u;
    k_u64_t off;
    volatile k_u8_t *mapped;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu bufread: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    b = gpu_amd_vram_find(&g_vram_mgr, id);
    if (b == NULL) {
        kshell_printf("gpu bufread: bad id %u\n", (unsigned)id);
        return 1;
    }
    if (!b->filled) {
        kshell_printf("gpu bufread: id=%u not filled\n", (unsigned)id);
        return 1;
    }
    if (!gpu_amd_get_bar0(&bar0_base, &bar0_size) || bar0_base == 0u) {
        kshell_printf("gpu bufread: BAR0 not available\n");
        return 1;
    }
    if (b->offset > bar0_size || b->size > bar0_size - b->offset) {
        kshell_printf("gpu bufread: block outside BAR0 aperture\n");
        return 1;
    }

    /* A write-combining store of a previous fill may still sit in the CPU
     * buffer; fence it so this readback sees the memory the card sees. */
    __asm__ volatile("mfence" : : : "memory");
    for (off = 0; off < b->size; off += GPU_AMD_VRAM_BLOCK_SIZE) {
        volatile k_u32_t *p;
        k_u64_t chunk = b->size - off;
        k_u32_t i;
        k_u32_t words;

        if (chunk > GPU_AMD_VRAM_BLOCK_SIZE) {
            chunk = GPU_AMD_VRAM_BLOCK_SIZE;
        }
        mapped = amd_map_vram(bar0_base, b->offset + off, (k_size_t)chunk);
        if (mapped == NULL) {
            kshell_printf("gpu bufread: map failed at +0x%x\n", (unsigned)(b->offset + off));
            return 1;
        }
        p = (volatile k_u32_t *)(uintptr_t)mapped;
        words = (k_u32_t)(chunk / 4u);
        for (i = 0; i < words; i++) {
            k_u32_t expect = gpu_amd_vram_pattern(b->pattern, b->offset, (off / 4u) + (k_u64_t)i);
            k_u32_t got = p[i];
            if (got != expect) {
                kshell_printf("gpu bufread: mismatch id=%u at +%u: got 0x%08x expect 0x%08x\n",
                              (unsigned)id, (unsigned)((off + (k_u64_t)i * 4u)), (unsigned)got,
                              (unsigned)expect);
                kerror("gpu bufread: mismatch id=%u", (unsigned)id);
                return 1;
            }
        }
    }

    kshell_printf("gpu bufread: id=%u name=%s pattern=0x%08x size=%u KiB OK\n", (unsigned)id,
                  b->name[0] != '\0' ? b->name : "-", (unsigned)b->pattern,
                  (unsigned)(b->size / 1024ull));
    kinfo("gpu bufread: id=%u ok", (unsigned)id);
    return 0;
}

int port_x86_64_gpu_amd_buffree(k_u32_t id)
{
    struct gpu_amd_vram_block *buffer;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu buffree: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    buffer = gpu_amd_vram_find(&g_vram_mgr, id);
    if (buffer == NULL) {
        kshell_printf("gpu buffree: bad id %u\n", (unsigned)id);
        return 1;
    }
    if ((s_amd_surface.mapped && s_amd_surface.id == id) ||
        (s_amd_flip.active && s_amd_flip.id == id)) {
        kshell_printf("gpu buffree: id=%u is mapped or active; unmap/restore first\n",
                      (unsigned)id);
        return 1;
    }
    rc = gpu_amd_vram_free(&g_vram_mgr, id);
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu buffree: id=%u: %s\n", (unsigned)id, gpu_amd_vram_err_name(rc));
        return 1;
    }
    kshell_printf("gpu buffree: id=%u freed\n", (unsigned)id);
    kinfo("gpu buffree: id=%u", (unsigned)id);
    return 0;
}

int port_x86_64_gpu_amd_buflist(void)
{
    int i;
    int used = 0;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu buflist: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }

    kshell_printf("gpu buflist: id offset size name filled mapped owner active\n");
    for (i = 0; i < GPU_AMD_VRAM_MAX_ALLOCS; i++) {
        const struct gpu_amd_vram_block *b = &g_vram_mgr.allocs[i];
        if (b->id == 0u || b->name[0] == '\0') {
            continue;
        }
        used++;
        kshell_printf("  id=%u off=0x%x%08x size=%uKiB name=%s filled=%s", (unsigned)b->id,
                      (unsigned)(b->offset >> 32), (unsigned)(k_u32_t)b->offset,
                      (unsigned)(b->size / 1024ull), b->name, b->filled ? "yes" : "no");
        if (b->filled) {
            kshell_printf(" pattern=0x%08x", (unsigned)b->pattern);
        }
        if (s_amd_surface.mapped && s_amd_surface.id == b->id) {
            kshell_printf(" mapped=va:0x%x%08x stride:%u format:%s",
                          (unsigned)((k_u64_t)(uintptr_t)s_amd_surface.surface.base >> 32),
                          (unsigned)(k_u32_t)(k_u64_t)(uintptr_t)s_amd_surface.surface.base,
                          (unsigned)s_amd_surface.surface.pitch,
                          gpu_amd_plane_format_name(s_amd_surface.plane.format));
        } else {
            kshell_printf(" mapped=no");
        }
        kshell_printf(" owner=%s",
                      s_amd_surface.mapped && s_amd_surface.id == b->id && s_amd_surface.fb_owned
                          ? "fb"
                          : "-");
        kshell_printf(" active=%s\n", s_amd_flip.active && s_amd_flip.id == b->id ? "yes" : "no");
    }
    if (used == 0) {
        kshell_printf("  (no buffers)\n");
    }
    return 0;
}

static bool amd_wait_vblank(k_u8_t pipe)
{
    k_u32_t off = gpu_amd_dcp_offset(pipe);
    k_u32_t reg = GPU_AMD_MMCRTC_STATUS + off;
    k_u64_t deadline = port_x86_64_ticks() + 200u;
    bool seen_clear = false;

    if (!amd_mmio_mapped(reg, (k_u32_t)port_x86_64_gpu_info()->mmio_size)) {
        return false;
    }

    /* Wait for a vblank edge: first leave vblank, then see it asserted. */
    while (port_x86_64_ticks() < deadline) {
        k_u32_t v = amd_mmio_read32(reg);
        bool in_vblank = (v & GPU_AMD_CRTC_STATUS__CRTC_V_BLANK_MASK) != 0u;

        if (!in_vblank) {
            seen_clear = true;
        } else if (seen_clear) {
            return true;
        }
    }

    /* Fallback: accept being in vblank if we never saw a clear edge. */
    return (amd_mmio_read32(reg) & GPU_AMD_CRTC_STATUS__CRTC_V_BLANK_MASK) != 0u;
}

/* A second, independent confirmation of the flip: the surface-update pending
 * bit is the one the AMD DC path polls after a GRPH address write. It may be
 * zero already when the update was applied immediately, so "clear" is not a
 * claim that a pending edge was seen. */
static bool amd_surface_update_applied(k_u32_t mmio_size)
{
    k_u64_t deadline = port_x86_64_ticks() + 200u;

    if (!amd_mmio_mapped(GPU_AMD_MMUNP_GRPH_UPDATE, mmio_size)) {
        return false;
    }
    while (port_x86_64_ticks() < deadline) {
        k_u32_t v = amd_mmio_read32(GPU_AMD_MMUNP_GRPH_UPDATE);

        if ((v & GPU_AMD_UNP_GRPH_UPDATE__GRPH_SURFACE_UPDATE_PENDING_MASK) == 0u) {
            return true;
        }
    }
    return false;
}

static volatile k_u8_t *amd_map_vram(k_u64_t bar0_base, k_u64_t offset, k_size_t bytes)
{
    k_u64_t aligned = offset & ~((k_u64_t)GPU_AMD_SURFACE_MAP_ALIGN - 1ull);
    k_u64_t delta = offset - aligned;
    k_u64_t span;
    k_u32_t pages;

    if (bytes == 0u || delta > (~(k_u64_t)0u) - (k_u64_t)bytes) {
        return NULL;
    }
    span = delta + (k_u64_t)bytes;
    pages = port_x86_64_paging_map_framebuffer(PORT_X86_64_GPU_VRAM_VIRT, bar0_base + aligned,
                                               (k_size_t)span);
    if (pages == 0u) {
        return NULL;
    }
    return (volatile k_u8_t *)(uintptr_t)(PORT_X86_64_GPU_VRAM_VIRT + delta);
}

static void amd_present_register(void);

static struct gpu_amd_scanout_region *amd_primary_region(struct gpu_amd_scanout_region *regions,
                                                         int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (regions[i].primary && regions[i].enabled) {
            return &regions[i];
        }
    }
    return NULL;
}

static void amd_plane_report(const char *action, const struct gpu_amd_scanout_region *plane)
{
    kshell_printf("%s: pipe %u %ux%u pitch=%u stride=%u bytes format=%s depth=%u code=%u "
                  "bpp=%u control=0x%08x tiling=0x%08x linear=%s\n",
                  action, (unsigned)plane->pipe, (unsigned)plane->pitch_pixels,
                  (unsigned)plane->height, (unsigned)plane->pitch_pixels,
                  (unsigned)((k_u64_t)plane->pitch_pixels * plane->bytes_per_pixel),
                  gpu_amd_plane_format_name(plane->format),
                  (unsigned)gpu_amd_reg_field(plane->control, GPU_AMD_GRPH_CONTROL__GRPH_DEPTH_MASK,
                                              GPU_AMD_GRPH_CONTROL__GRPH_DEPTH_SHIFT),
                  (unsigned)gpu_amd_reg_field(plane->control,
                                              GPU_AMD_GRPH_CONTROL__GRPH_FORMAT_MASK,
                                              GPU_AMD_GRPH_CONTROL__GRPH_FORMAT_SHIFT),
                  (unsigned)plane->bytes_per_pixel, (unsigned)plane->control,
                  (unsigned)plane->tiling_control, plane->linear ? "yes" : "no");
    kinfo("gpu: %s plane pipe=%u pitch=%u height=%u format=%s bpp=%u control=0x%08x "
          "tiling=0x%08x linear=%s",
          action, (unsigned)plane->pipe, (unsigned)plane->pitch_pixels, (unsigned)plane->height,
          gpu_amd_plane_format_name(plane->format), (unsigned)plane->bytes_per_pixel,
          (unsigned)plane->control, (unsigned)plane->tiling_control, plane->linear ? "yes" : "no");
}

static k_u64_t amd_surface_address(k_u64_t original, k_u64_t bar0_base, k_u64_t vram_size,
                                   k_u64_t offset)
{
    if (original >= bar0_base && original - bar0_base < vram_size) {
        return bar0_base + offset;
    }
    return offset;
}

static bool amd_plane_points_to(const struct gpu_amd_scanout_region *plane, k_u64_t bar0_base,
                                k_u64_t vram_size, k_u64_t offset)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    k_u32_t dcp = gpu_amd_dcp_offset(plane->pipe);
    k_u64_t expected = amd_surface_address(plane->register_address, bar0_base, vram_size, offset);
    k_u32_t lo;
    k_u32_t hi;
    k_u32_t pitch;
    k_u32_t control;
    k_u32_t tiling;

    if (info == NULL ||
        !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp, info->mmio_size) ||
        !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp, info->mmio_size) ||
        !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_PITCH + dcp, info->mmio_size) ||
        !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_CONTROL + dcp, info->mmio_size) ||
        !amd_mmio_mapped(GPU_AMD_MMDCP_UNP_GRPH_CONTROL, info->mmio_size)) {
        return false;
    }

    lo = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp);
    hi = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp);
    pitch = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PITCH + dcp);
    control = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_CONTROL + dcp);
    tiling = amd_mmio_read32(GPU_AMD_MMDCP_UNP_GRPH_CONTROL);
    return lo == (k_u32_t)expected && hi == (k_u32_t)(expected >> 32) &&
           (pitch & GPU_AMD_GRPH_PITCH__GRPH_PITCH_MASK) == plane->pitch_pixels &&
           control == plane->control && tiling == plane->tiling_control && plane->linear &&
           gpu_amd_plane_is_linear(tiling);
}

static bool amd_restore_plane(void)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    k_u32_t dcp;
    k_u32_t lo;
    k_u32_t hi;
    k_u32_t pitch;
    k_u32_t control;
    k_u32_t tiling;

    if (!s_amd_flip.active) {
        return true;
    }
    if (info == NULL || info->mmio_base == 0u || info->mmio_size == 0u) {
        return false;
    }
    dcp = gpu_amd_dcp_offset(s_amd_flip.pipe);
    lo = (k_u32_t)s_amd_flip.original_address;
    hi = (k_u32_t)(s_amd_flip.original_address >> 32);

    kinfo("gpu: restoring GRPH pipe %u to 0x%x%08x", (unsigned)s_amd_flip.pipe, (unsigned)hi,
          (unsigned)lo);
    k_logfile_flush();
    amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp, lo);
    amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp, hi);
    amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PITCH + dcp, s_amd_flip.original_pitch);
    if (!amd_wait_vblank(s_amd_flip.pipe) || !amd_surface_update_applied(info->mmio_size)) {
        kerror("gpu: restore vblank/surface-update timeout");
        return false;
    }

    lo = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp);
    hi = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp);
    pitch = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PITCH + dcp);
    control = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_CONTROL + dcp);
    tiling = amd_mmio_read32(GPU_AMD_MMDCP_UNP_GRPH_CONTROL);
    if (lo != (k_u32_t)s_amd_flip.original_address ||
        hi != (k_u32_t)(s_amd_flip.original_address >> 32) || pitch != s_amd_flip.original_pitch ||
        control != s_amd_flip.original_control || tiling != s_amd_flip.original_tiling) {
        kerror("gpu: restore readback mismatch");
        return false;
    }

    s_amd_flip.active = false;
    s_amd_flip.id = 0u;
    s_amd_flip.active_offset = 0u;
    return true;
}

static int amd_flip_to_offset(k_u32_t id, k_u64_t offset, k_u64_t bytes, bool confirmed, bool keep,
                              const char *label, bool quiet)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    struct gpu_amd_scanout_region regions[GPU_AMD_DCE_PIPE_COUNT * 2];
    struct gpu_amd_scanout_region *plane;
    k_u64_t bar0_base = 0u;
    k_u64_t bar0_size = 0u;
    k_u64_t aperture_size;
    k_u64_t frame_bytes;
    k_u64_t new_addr;
    k_u32_t dcp;
    k_u32_t orig_pitch;
    k_u32_t new_lo;
    k_u32_t new_hi;
    int count;
    int rc;

    if (info == NULL || !info->present || info->vendor != 0x1002u || info->mmio_base == 0u ||
        info->mmio_size == 0u || !gpu_amd_get_bar0(&bar0_base, &bar0_size)) {
        if (!quiet) {
            kshell_printf("gpu %s: AMD MMIO/BAR0 not available\n", label);
        }
        return 1;
    }

    count = read_scanout_regions(regions, (int)(sizeof(regions) / sizeof(regions[0])));
    plane = amd_primary_region(regions, count);
    if (plane == NULL) {
        if (!quiet) {
            kshell_printf("gpu %s: no enabled primary GRPH scanout found\n", label);
        }
        return 1;
    }
    if (!quiet) {
        amd_plane_report(label, plane);
    }

    frame_bytes = gpu_amd_frame_bytes(plane->pitch_pixels, plane->height, plane->bytes_per_pixel);
    aperture_size = bar0_size < info->vram_bytes ? bar0_size : info->vram_bytes;
    if (frame_bytes == 0u || !plane->linear || !plane->address_valid ||
        plane->address > aperture_size || frame_bytes > aperture_size - plane->address ||
        offset < GPU_AMD_VRAM_RESERVED_END || offset > aperture_size ||
        frame_bytes > aperture_size - offset || bytes < frame_bytes) {
        if (!quiet) {
            kshell_printf("gpu %s: invalid/unknown plane or buffer does not fit %u KiB frame\n",
                          label, (unsigned)(frame_bytes / 1024ull));
        }
        return 1;
    }

    dcp = gpu_amd_dcp_offset(plane->pipe);
    orig_pitch = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PITCH + dcp);
    new_addr = amd_surface_address(plane->register_address, bar0_base, info->vram_bytes, offset);
    new_lo = (k_u32_t)new_addr;
    new_hi = (k_u32_t)(new_addr >> 32);

    if (!confirmed) {
        if (!quiet) {
            kshell_printf("gpu %s: dry-run, target BAR0+0x%x%08x; pass --yes to write GRPH\n",
                          label, (unsigned)(offset >> 32), (unsigned)(k_u32_t)offset);
        }
        return 0;
    }

    if (s_amd_flip.active) {
        if (s_amd_flip.pipe != plane->pipe) {
            if (!quiet) {
                kshell_printf("gpu %s: active pipe changed; refusing a second plane\n", label);
            }
            return 1;
        }
        if (s_amd_flip.active_offset == offset &&
            amd_plane_points_to(plane, bar0_base, info->vram_bytes, offset)) {
            if (keep) {
                s_amd_flip.id = id;
                return 0;
            }
            return amd_restore_plane() ? 0 : 1;
        }
        if (!amd_plane_points_to(plane, bar0_base, info->vram_bytes, s_amd_flip.active_offset)) {
            (void)amd_restore_plane();
            return 1;
        }
    } else if (keep && amd_plane_points_to(plane, bar0_base, info->vram_bytes, offset)) {
        return 0;
    }

    if (!s_amd_flip.active) {
        s_amd_flip.original_address = plane->register_address;
        s_amd_flip.original_pitch = orig_pitch;
        s_amd_flip.original_control = plane->control;
        s_amd_flip.original_tiling = plane->tiling_control;
        s_amd_flip.pipe = plane->pipe;
        s_amd_flip.active = true;
    }
    s_amd_flip.id = id;
    s_amd_flip.active_offset = offset;

    kinfo("gpu: %s: about to point GRPH pipe %u at BAR0+0x%x%08x", label, (unsigned)plane->pipe,
          (unsigned)(offset >> 32), (unsigned)(k_u32_t)offset);
    if (!quiet) {
        kshell_printf("gpu: %s: about to point GRPH at BAR0+0x%x%08x\n", label,
                      (unsigned)(offset >> 32), (unsigned)(k_u32_t)offset);
    }
    k_logfile_flush();
    __asm__ volatile("mfence" : : : "memory");

    /* Only the surface address and unchanged pitch are touched. No timings,
     * PLL, DDI, format, tiling or enable state is changed. */
    amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp, new_lo);
    amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp, new_hi);
    amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PITCH + dcp, orig_pitch);

    if (!amd_wait_vblank(plane->pipe) || !amd_surface_update_applied(info->mmio_size) ||
        !amd_plane_points_to(plane, bar0_base, info->vram_bytes, offset)) {
        kerror("gpu: %s flip readback/vblank/surface-update failed", label);
        (void)amd_restore_plane();
        return 1;
    }

    if (keep) {
        kinfo("gpu: %s keeps BAR0+0x%x%08x active", label, (unsigned)(offset >> 32),
              (unsigned)(k_u32_t)offset);
        return 0;
    }

    rc = amd_restore_plane() ? 0 : 1;
    if (!quiet) {
        kshell_printf("gpu %s: %s\n", label,
                      rc == 0 ? "restored original surface" : "WARNING: restore not confirmed");
    }
    return rc;
}

static int amd_flip_to_buffer(k_u32_t id, bool confirmed, bool keep, const char *label, bool quiet)
{
    struct gpu_amd_vram_block *buffer;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        if (!quiet) {
            kshell_printf("gpu %s: %s\n", label, gpu_amd_vram_err_name(rc));
        }
        return 1;
    }
    buffer = gpu_amd_vram_find(&g_vram_mgr, id);
    if (buffer == NULL) {
        if (!quiet) {
            kshell_printf("gpu %s: bad id %u\n", label, (unsigned)id);
        }
        return 1;
    }
    if (gpu_amd_vram_reserved_overlap(&g_vram_mgr, buffer->offset, buffer->size)) {
        if (!quiet) {
            kshell_printf("gpu %s: buffer overlaps reserved/scanout\n", label);
        }
        return 1;
    }
    return amd_flip_to_offset(id, buffer->offset, buffer->size, confirmed, keep, label, quiet);
}

static void amd_surface_report(const char *action, k_u32_t id, bool armed)
{
    const struct gpu_amd_vram_block *buffer = gpu_amd_vram_find(&g_vram_mgr, id);
    const struct gpu_amd_surface_slot *slot = &s_amd_surface;

    kshell_printf(
        "gpu %s: id=%u name=%s va=0x%x%08x size=%u KiB stride=%u px (%u bytes) "
        "surface=%u KiB format=%s owner=%s present=%s\n",
        action, (unsigned)id, buffer != NULL && buffer->name[0] != '\0' ? buffer->name : "-",
        (unsigned)((k_u64_t)(uintptr_t)slot->surface.base >> 32),
        (unsigned)(k_u32_t)(k_u64_t)(uintptr_t)slot->surface.base,
        (unsigned)((buffer != NULL ? buffer->size : slot->map_bytes) / 1024ull),
        (unsigned)slot->surface.pitch,
        (unsigned)((k_u64_t)slot->surface.pitch * (k_u64_t)slot->plane.bytes_per_pixel),
        (unsigned)(slot->surface_bytes / 1024ull), gpu_amd_plane_format_name(slot->plane.format),
        slot->fb_owned ? "fb" : "kernel", armed ? "VRAM flip (armed)" : "RAM copy (not armed)");
}

int port_x86_64_gpu_amd_bufmap(k_u32_t id, bool allow_present, bool own)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    struct gpu_amd_scanout_region regions[GPU_AMD_DCE_PIPE_COUNT * 2];
    struct gpu_amd_scanout_region *plane;
    struct gpu_amd_vram_block *buffer;
    struct gpu_amd_surface_slot plan;
    struct fb_info framebuffer;
    k_u64_t bar0_base = 0u;
    k_u64_t bar0_size = 0u;
    k_u32_t pages;
    int count;
    int rc;

    rc = gpu_amd_vram_ensure_init();
    if (rc != GPU_AMD_VRAM_OK) {
        kshell_printf("gpu bufmap: %s\n", gpu_amd_vram_err_name(rc));
        return 1;
    }
    buffer = gpu_amd_vram_find(&g_vram_mgr, id);
    if (buffer == NULL) {
        kshell_printf("gpu bufmap: bad id %u\n", (unsigned)id);
        return 1;
    }
    if (gpu_amd_vram_reserved_overlap(&g_vram_mgr, buffer->offset, buffer->size)) {
        kshell_printf("gpu bufmap: buffer overlaps reserved/scanout\n");
        return 1;
    }
    if (info == NULL || !gpu_amd_get_bar0(&bar0_base, &bar0_size)) {
        kshell_printf("gpu bufmap: BAR0 not available\n");
        return 1;
    }

    count = read_scanout_regions(regions, (int)(sizeof(regions) / sizeof(regions[0])));
    plane = amd_primary_region(regions, count);
    if (plane == NULL) {
        kshell_printf("gpu bufmap: no enabled primary GRPH scanout found\n");
        return 1;
    }
    amd_plane_report("gpu bufmap", plane);
    if (!port_x86_64_fb_info(&framebuffer) ||
        (framebuffer.format != FB_FORMAT_RGB565 && framebuffer.format != FB_FORMAT_XRGB8888)) {
        kshell_printf("gpu bufmap: unsupported port framebuffer format %u; CPU surface refused\n",
                      (unsigned)framebuffer.format);
        return 1;
    }

    rc = gpu_amd_surface_plan(buffer, plane, framebuffer.width, framebuffer.height, bar0_base,
                              bar0_size, info->vram_bytes, PORT_X86_64_GPU_VRAM_SURFACE_VIRT,
                              PORT_X86_64_GPU_VRAM_SURFACE_BYTES, &plan);
    if (rc != GPU_AMD_SURFACE_OK) {
        kshell_printf("gpu bufmap: %s (plane %s, %u bpp)\n", gpu_amd_surface_err_name(rc),
                      gpu_amd_plane_format_name(plane->format), (unsigned)plane->bytes_per_pixel);
        return 1;
    }
    if (plan.surface.format != framebuffer.format) {
        kshell_printf("gpu bufmap: plane %s (%u bpp) differs from framebuffer format %u; "
                      "no copy path, RAM copy stays in use\n",
                      gpu_amd_plane_format_name(plane->format), (unsigned)plane->bytes_per_pixel,
                      (unsigned)framebuffer.format);
        kerror("gpu bufmap: plane/framebuffer format mismatch, no VRAM surface");
        return 1;
    }
    if (plan.surface.pitch != framebuffer.pitch) {
        kshell_printf("gpu bufmap: plane stride=%u differs from framebuffer stride=%u; "
                      "RAM copy stays in use\n",
                      (unsigned)plan.surface.pitch, (unsigned)framebuffer.pitch);
        return 1;
    }

    if (s_amd_surface.mapped) {
        if (s_amd_surface.id != id) {
            kshell_printf("gpu bufmap: id=%u is already mapped; unmap it first\n",
                          (unsigned)s_amd_surface.id);
            return 1;
        }
        if (s_amd_surface.map_paddr != plan.map_paddr ||
            s_amd_surface.map_bytes != plan.map_bytes ||
            s_amd_surface.surface.width != plan.surface.width ||
            s_amd_surface.surface.height != plan.surface.height ||
            s_amd_surface.surface.pitch != plan.surface.pitch ||
            s_amd_surface.plane.control != plan.plane.control ||
            s_amd_surface.plane.tiling_control != plan.plane.tiling_control) {
            kshell_printf("gpu bufmap: plane geometry changed; unmap id=%u first\n", (unsigned)id);
            return 1;
        }
        if (s_amd_flip.active && s_amd_surface.fb_owned != own) {
            kshell_printf("gpu bufmap: cannot change ownership of an active id=%u\n", (unsigned)id);
            return 1;
        }
        s_amd_surface.fb_owned = own;
        if (allow_present && !s_amd_surface_armed) {
            s_amd_surface_armed = true;
            amd_present_register();
        }
        amd_surface_report("bufmap", id, s_amd_surface_armed);
        return 0;
    }

    pages = port_x86_64_paging_map_framebuffer(plan.map_vaddr, plan.map_paddr,
                                               (k_size_t)plan.map_bytes);
    if (pages == 0u) {
        kshell_printf("gpu bufmap: fixed VA mapping failed for %u KiB\n",
                      (unsigned)(plan.map_bytes / 1024ull));
        return 1;
    }
    plan.mapped = true;
    plan.fb_owned = own;
    s_amd_surface = plan;
    s_amd_surface_armed = allow_present;
    amd_present_register();
    amd_surface_report("bufmap", id, s_amd_surface_armed);
    kinfo("gpu: bufmap id=%u va=0x%x%08x BAR0+0x%x%08x map=%uKiB stride=%u format=%s", (unsigned)id,
          (unsigned)((k_u64_t)(uintptr_t)s_amd_surface.surface.base >> 32),
          (unsigned)(k_u32_t)(k_u64_t)(uintptr_t)s_amd_surface.surface.base,
          (unsigned)(buffer->offset >> 32), (unsigned)(k_u32_t)buffer->offset,
          (unsigned)(s_amd_surface.map_bytes / 1024ull), (unsigned)s_amd_surface.surface.pitch,
          gpu_amd_plane_format_name(plane->format));
    return 0;
}

int port_x86_64_gpu_amd_bufunmap(k_u32_t id, bool confirmed)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    struct gpu_amd_scanout_region regions[GPU_AMD_DCE_PIPE_COUNT * 2];
    struct gpu_amd_scanout_region *plane;
    struct gpu_amd_vram_block *buffer;
    k_u64_t bar0_base = 0u;
    k_u64_t bar0_size = 0u;
    bool displayed = false;
    int count;

    if (!s_amd_surface.mapped || s_amd_surface.id != id) {
        kshell_printf("gpu bufunmap: id=%u is not mapped\n", (unsigned)id);
        return 1;
    }
    buffer = gpu_amd_vram_find(&g_vram_mgr, id);
    if (buffer == NULL) {
        kshell_printf("gpu bufunmap: bad id %u\n", (unsigned)id);
        return 1;
    }
    displayed = s_amd_flip.active && s_amd_flip.active_offset == buffer->offset;
    if (info != NULL && gpu_amd_get_bar0(&bar0_base, &bar0_size)) {
        count = read_scanout_regions(regions, (int)(sizeof(regions) / sizeof(regions[0])));
        plane = amd_primary_region(regions, count);
        displayed =
            displayed || (plane != NULL &&
                          amd_plane_points_to(plane, bar0_base, info->vram_bytes, buffer->offset));
    }
    if (displayed && !s_amd_flip.active) {
        kshell_printf("gpu bufunmap: scanout uses id=%u but no rollback state exists; refusing\n",
                      (unsigned)id);
        return 1;
    }
    if (displayed && !confirmed) {
        kshell_printf("gpu bufunmap: scanout still uses id=%u; pass --yes to restore first\n",
                      (unsigned)id);
        return 1;
    }
    if (displayed && !amd_restore_plane()) {
        kshell_printf("gpu bufunmap: WARNING restore not confirmed; mapping kept\n");
        return 1;
    }

    port_x86_64_paging_unmap_device(s_amd_surface.map_vaddr, (k_size_t)s_amd_surface.map_bytes);
    gpu_amd_surface_reset(&s_amd_surface);
    s_amd_surface_armed = false;
    kshell_printf("gpu bufunmap: id=%u unmapped, block kept; use gpu buffree to release\n",
                  (unsigned)id);
    kinfo("gpu: bufmap id=%u unmapped", (unsigned)id);
    return 0;
}

static bool amd_present_plane_compatible(void)
{
    struct gpu_amd_scanout_region regions[GPU_AMD_DCE_PIPE_COUNT * 2];
    struct gpu_amd_scanout_region *plane;
    int count;

    if (!s_amd_surface.mapped) {
        return false;
    }
    count = read_scanout_regions(regions, (int)(sizeof(regions) / sizeof(regions[0])));
    plane = amd_primary_region(regions, count);
    return plane != NULL && plane->pipe == s_amd_surface.plane.pipe &&
           plane->format == s_amd_surface.plane.format &&
           plane->bytes_per_pixel == s_amd_surface.plane.bytes_per_pixel &&
           plane->pitch_pixels == s_amd_surface.plane.pitch_pixels &&
           plane->height == s_amd_surface.plane.height &&
           plane->control == s_amd_surface.plane.control && plane->linear &&
           plane->tiling_control == s_amd_surface.plane.tiling_control;
}

static int amd_present(void *context, const struct k_fb_surface *dst,
                       const struct k_fb_surface *src)
{
    struct gpu_amd_vram_block *buffer;
    enum gpu_amd_present_route route;
    k_u64_t bar0_base = 0u;
    k_u64_t bar0_size = 0u;
    const struct gpu_info *info = port_x86_64_gpu_info();

    (void)context;
    route = gpu_amd_present_select(&s_amd_surface, dst, src);
    if (route == GPU_AMD_PRESENT_RAM) {
        if (s_amd_flip.active && !amd_restore_plane()) {
            return KERR_IO;
        }
        return KERR_NOTSUP;
    }

    buffer = gpu_amd_vram_find(&g_vram_mgr, s_amd_surface.id);
    if (buffer == NULL || !amd_present_plane_compatible() || info == NULL ||
        !gpu_amd_get_bar0(&bar0_base, &bar0_size)) {
        if (s_amd_flip.active && !amd_restore_plane()) {
            return KERR_IO;
        }
        return KERR_NOTSUP;
    }

    if (s_amd_flip.active && s_amd_flip.active_offset == buffer->offset) {
        if (amd_flip_to_buffer(s_amd_surface.id, true, true, "present", true) == 0) {
            return 0;
        }
        return s_amd_flip.active ? KERR_IO : KERR_NOTSUP;
    }
    if (!s_amd_surface_armed) {
        if (s_amd_flip.active && !amd_restore_plane()) {
            return KERR_IO;
        }
        return KERR_NOTSUP;
    }
    if (amd_flip_to_buffer(s_amd_surface.id, true, true, "present", true) != 0) {
        return s_amd_flip.active ? KERR_IO : KERR_NOTSUP;
    }
    return 0;
}

static int amd_present_release(void *context)
{
    k_u32_t id;
    int rc;

    (void)context;
    if (!amd_restore_plane()) {
        return KERR_IO;
    }
    s_amd_surface_armed = false;
    if (!gpu_amd_surface_should_free(&s_amd_surface, true)) {
        return 0;
    }

    /* The last descriptor of /dev/fb0 is also closed when a program dies:
     * task_exit closes its file table. An owned block goes back to the free
     * pool here; a failed restore above keeps both the mapping and the block. */
    id = s_amd_surface.id;
    port_x86_64_paging_unmap_device(s_amd_surface.map_vaddr, (k_size_t)s_amd_surface.map_bytes);
    gpu_amd_surface_reset(&s_amd_surface);
    rc = gpu_amd_vram_free(&g_vram_mgr, id);
    if (rc != GPU_AMD_VRAM_OK) {
        kerror("gpu: fb-owned block id=%u not freed: %s", (unsigned)id, gpu_amd_vram_err_name(rc));
        return KERR_IO;
    }
    kshell_printf("gpu: released fb-owned VRAM block id=%u\n", (unsigned)id);
    kinfo("gpu: released fb-owned VRAM block id=%u", (unsigned)id);
    return 0;
}

static const struct k_fb_ops s_amd_present_ops = {
    .present = amd_present,
    .present_release = amd_present_release,
};

static void amd_present_register(void)
{
    (void)k_fb_ops_register(&s_amd_present_ops, NULL);
    kinfo("gpu: AMD VRAM surface backend registered (RGB565/XRGB8888)");
}

int port_x86_64_gpu_amd_bufflip(k_u32_t id, bool confirmed, bool keep)
{
    return amd_flip_to_buffer(id, confirmed, keep, "bufflip", false) == 0 ? 0 : 1;
}

int port_x86_64_gpu_amd_amdflip(bool confirmed, bool keep)
{
    const struct gpu_info *info = port_x86_64_gpu_info();
    struct gpu_amd_scanout_region regions[12];
    struct gpu_amd_scanout_region *target = NULL;
    int region_count = 0;
    int i;
    k_u64_t bar0_base = 0u;
    k_u64_t frame_bytes;
    k_u64_t frame_offset;
    k_u32_t orig_lo;
    k_u32_t orig_hi;
    k_u32_t orig_pitch;
    k_u32_t new_pitch;
    k_u8_t *staging = NULL;
    volatile k_u8_t *vram = NULL;
    bool wrote_grph = false;
    bool restore_ok = false;
    bool ok = false;

    if (info == NULL || !info->present) {
        kshell_printf("gpu amdflip: no display controller present\n");
        return 1;
    }
    if (info->vendor != 0x1002u) {
        kshell_printf("gpu amdflip: not an AMD controller (%04x:%04x)\n", (unsigned)info->vendor,
                      (unsigned)info->device);
        return 1;
    }
    if (info->mmio_base == 0u || info->mmio_size == 0u) {
        kshell_printf("gpu amdflip: BAR5 MMIO not mapped\n");
        return 1;
    }

    {
        struct port_x86_64_pci_device pci;
        pci.bus = info->bus;
        pci.slot = info->slot;
        pci.function = info->function;
        bar0_base = bar_base(&pci, PCI_BAR0);
    }

    if (bar0_base == 0u || info->vram_bytes == 0u) {
        kshell_printf("gpu amdflip: BAR0 aperture unknown\n");
        return 1;
    }

    region_count = read_scanout_regions(regions, (int)(sizeof(regions) / sizeof(regions[0])));
    for (i = 0; i < region_count; i++) {
        if (regions[i].primary && regions[i].enabled) {
            target = &regions[i];
            break;
        }
    }
    if (target == NULL) {
        kshell_printf("gpu amdflip: no enabled primary GRPH scanout found\n");
        kerror("gpu amdflip: no primary scanout");
        return 1;
    }
    amd_plane_report("gpu amdflip", target);
    if (!target->linear || !target->address_valid) {
        kshell_printf(
            "gpu amdflip: tiled/unknown plane or address refused; RAM copy stays in use\n");
        return 1;
    }

    frame_bytes =
        gpu_amd_frame_bytes(target->pitch_pixels, target->height, target->bytes_per_pixel);
    if (frame_bytes == 0u) {
        kshell_printf("gpu amdflip: invalid frame size (pitch %u, height %u)\n",
                      (unsigned)target->pitch_pixels, (unsigned)target->height);
        return 1;
    }

    frame_offset =
        gpu_amd_choose_frame_offset(info->vram_bytes, frame_bytes, regions, region_count);
    if (frame_offset == 0u) {
        kshell_printf("gpu amdflip: no safe VRAM block for %u KiB frame\n",
                      (unsigned)(frame_bytes / 1024ull));
        kerror("gpu amdflip: no safe VRAM block for %u bytes", (unsigned)frame_bytes);
        return 1;
    }

    orig_lo = (k_u32_t)(target->register_address & 0xFFFFFFFFu);
    orig_hi = (k_u32_t)(target->register_address >> 32);
    orig_pitch = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PITCH + gpu_amd_dcp_offset(target->pipe));
    new_pitch = orig_pitch; /* preserve other pitch bits; address is the flip */

    kshell_printf("gpu amdflip: pipe %u %ux%u pitch %u, frame %u KiB\n", (unsigned)target->pipe,
                  (unsigned)target->pitch_pixels, (unsigned)target->height,
                  (unsigned)(orig_pitch & GPU_AMD_GRPH_PITCH__GRPH_PITCH_MASK),
                  (unsigned)(frame_bytes / 1024ull));
    kshell_printf("gpu amdflip: source 0x%x%08x, target BAR0+0x%x%08x\n", (unsigned)orig_hi,
                  (unsigned)orig_lo, (unsigned)(frame_offset >> 32),
                  (unsigned)(k_u32_t)frame_offset);

    if (!confirmed) {
        kshell_printf("gpu amdflip: dry-run, nothing written; pass --yes to flip on the laptop\n");
        return 0;
    }

    staging = (k_u8_t *)kmm_alloc((k_size_t)frame_bytes, KMM_PRIORITY_KERNEL);
    if (staging == NULL) {
        kshell_printf("gpu amdflip: no RAM for %u KiB staging buffer\n",
                      (unsigned)(frame_bytes / 1024ull));
        return 1;
    }

    kinfo("gpu: amdflip: copying frame into BAR0+0x%x%08x", (unsigned)(frame_offset >> 32),
          (unsigned)(k_u32_t)frame_offset);
    k_logfile_flush();

    /* The scanout reader normalized the source to a BAR0 offset. */
    {
        k_u64_t src = target->address;
        /* Copy current frame from source VRAM surface into RAM staging. */
        vram = amd_map_vram(bar0_base, src, (k_size_t)frame_bytes);
        if (vram == NULL) {
            kshell_printf("gpu amdflip: could not map source VRAM\n");
            goto out;
        }
        memcpy(staging, (const void *)vram, (k_size_t)frame_bytes);
    }

    /* Copy staging into our VRAM block. */
    vram = amd_map_vram(bar0_base, frame_offset, (k_size_t)frame_bytes);
    if (vram == NULL) {
        kshell_printf("gpu amdflip: could not map target VRAM\n");
        goto out;
    }
    memcpy((void *)vram, staging, (k_size_t)frame_bytes);
    __asm__ volatile("mfence" : : : "memory");

    /* Verify the VRAM copy before touching scanout registers. */
    if (memcmp((const void *)vram, staging, (k_size_t)frame_bytes) != 0) {
        kshell_printf("gpu amdflip: VRAM copy mismatch before flip\n");
        kerror("gpu amdflip: VRAM copy mismatch before flip");
        goto out;
    }

    kinfo("gpu: amdflip: about to point GRPH pipe %u at BAR0+0x%x%08x", (unsigned)target->pipe,
          (unsigned)(frame_offset >> 32), (unsigned)(k_u32_t)frame_offset);
    kshell_printf("gpu: amdflip: about to point GRPH at BAR0+0x%x%08x\n",
                  (unsigned)(frame_offset >> 32), (unsigned)(k_u32_t)frame_offset);
    k_logfile_flush();
    __asm__ volatile("mfence" : : : "memory");

    /* Write only GRPH address/pitch. No timings, PLL, DDI. */
    {
        k_u32_t dcp = gpu_amd_dcp_offset(target->pipe);
        k_u64_t new_addr = frame_offset;
        k_u32_t new_lo;
        k_u32_t new_hi;

        /* Preserve the original address format (BAR0 bus address or VRAM offset). */
        if (target->register_address >= bar0_base &&
            target->register_address - bar0_base < info->vram_bytes) {
            new_addr = bar0_base + frame_offset;
        }
        new_lo = (k_u32_t)(new_addr & 0xFFFFFFFFu);
        new_hi = (k_u32_t)(new_addr >> 32);

        wrote_grph = true;
        amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp, new_lo);
        amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp, new_hi);
        amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PITCH + dcp, new_pitch);

        if (!amd_wait_vblank(target->pipe)) {
            kshell_printf("gpu amdflip: vblank wait timed out after flip\n");
            kerror("gpu amdflip: vblank timeout after flip");
            goto restore;
        }

        /* Readback: confirm the surface address took effect. */
        {
            k_u32_t rb_lo = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp);
            k_u32_t rb_hi = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp);
            k_u32_t rb_pitch = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PITCH + dcp);
            if (rb_lo != new_lo || rb_hi != new_hi || rb_pitch != new_pitch) {
                kshell_printf("gpu amdflip: GRPH readback mismatch after flip\n");
                kerror("gpu amdflip: readback mismatch lo=%08x/%08x hi=%08x/%08x pitch=%08x/%08x",
                       (unsigned)rb_lo, (unsigned)new_lo, (unsigned)rb_hi, (unsigned)new_hi,
                       (unsigned)rb_pitch, (unsigned)new_pitch);
                goto restore;
            }
        }
    }

    /* Verify the displayed block still matches the saved frame. */
    vram = amd_map_vram(bar0_base, frame_offset, (k_size_t)frame_bytes);
    if (vram == NULL) {
        kshell_printf("gpu amdflip: could not remap target for verify\n");
        goto restore;
    }
    if (memcmp((const void *)vram, staging, (k_size_t)frame_bytes) != 0) {
        kshell_printf("gpu amdflip: picture verify FAILED (VRAM differs from staging)\n");
        kerror("gpu amdflip: picture verify failed");
        goto restore;
    }

    kshell_printf("gpu amdflip: GRPH readback OK, picture verified same\n");
    kinfo("gpu amdflip: GRPH readback OK, picture verified same at BAR0+0x%x%08x",
          (unsigned)(frame_offset >> 32), (unsigned)(k_u32_t)frame_offset);

    if (keep) {
        kshell_printf(
            "gpu amdflip: --keep: leaving our surface active (restore manually if needed)\n");
        kinfo("gpu amdflip: keeping surface at BAR0+0x%x%08x", (unsigned)(frame_offset >> 32),
              (unsigned)(k_u32_t)frame_offset);
        if (!s_amd_flip.active) {
            s_amd_flip.original_address = target->register_address;
            s_amd_flip.original_pitch = orig_pitch;
            s_amd_flip.original_control = target->control;
            s_amd_flip.original_tiling = target->tiling_control;
            s_amd_flip.pipe = target->pipe;
            s_amd_flip.active = true;
        }
        s_amd_flip.id = 0u;
        s_amd_flip.active_offset = frame_offset;
        ok = true;
        wrote_grph = false; /* intentional: do not restore */
        goto out;
    }

restore:
    if (wrote_grph) {
        k_u32_t dcp = gpu_amd_dcp_offset(target->pipe);
        kinfo("gpu: amdflip: restoring GRPH pipe %u to 0x%x%08x", (unsigned)target->pipe,
              (unsigned)orig_hi, (unsigned)orig_lo);
        k_logfile_flush();
        amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp, orig_lo);
        amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp, orig_hi);
        amd_mmio_write32(GPU_AMD_MMDCP_GRPH_PITCH + dcp, orig_pitch);
        if (amd_wait_vblank(target->pipe)) {
            k_u32_t rb_lo = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + dcp);
            k_u32_t rb_hi = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + dcp);
            restore_ok = (rb_lo == orig_lo && rb_hi == orig_hi);
        } else {
            restore_ok = false;
        }
        if (restore_ok) {
            kshell_printf("gpu amdflip: restored original surface 0x%x%08x\n", (unsigned)orig_hi,
                          (unsigned)orig_lo);
            kinfo("gpu amdflip: restored original surface");
        } else {
            kshell_printf("gpu amdflip: WARNING restore not confirmed\n");
            kerror("gpu amdflip: restore not confirmed");
        }
    }

    ok = wrote_grph ? restore_ok : false;
    /* If we never wrote GRPH (failed before write), it's not "ok" as a flip,
     * but we didn't break anything; return error. If we wrote and restored, ok. */

out:
    if (staging != NULL) {
        kmm_free(staging);
    }
    if (!confirmed) {
        return 0;
    }
    if (keep && ok) {
        return 0;
    }
    if (wrote_grph) {
        return ok ? 0 : 1;
    }
    return 1;
}
