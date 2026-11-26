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

#include "kernel/log.h"
#include "kshell/kshell.h"
#include "port_x86_64.h"

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

    if (out == NULL || max_out <= 0) {
        return 0;
    }

    for (pipe = 0; pipe < GPU_AMD_DCE_PIPE_COUNT && n < max_out; pipe++) {
        k_u32_t off = gpu_amd_dcp_offset(pipe);
        k_u32_t enable_reg = GPU_AMD_MMDCP_GRPH_ENABLE + off;
        k_u32_t enable;
        k_u32_t pitch;
        k_u32_t y_end;
        k_u32_t lo;
        k_u32_t hi;
        k_u64_t addr;
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
            !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_PITCH + off,
                             (k_u32_t)port_x86_64_gpu_info()->mmio_size) ||
            !amd_mmio_mapped(GPU_AMD_MMDCP_GRPH_Y_END + off,
                             (k_u32_t)port_x86_64_gpu_info()->mmio_size)) {
            continue;
        }

        lo = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS + off);
        hi = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off);
        pitch = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_PITCH + off);
        y_end = amd_mmio_read32(GPU_AMD_MMDCP_GRPH_Y_END + off);
        addr = ((k_u64_t)hi << 32) | (k_u64_t)lo;
        size = gpu_amd_scanout_size(pitch, y_end);

        out[n].pipe = pipe;
        out[n].primary = true;
        out[n].enabled = true;
        out[n].address = addr;
        out[n].pitch_pixels = pitch & GPU_AMD_GRPH_PITCH__GRPH_PITCH_MASK;
        out[n].height = (y_end & GPU_AMD_GRPH_Y_END__GRPH_Y_END_MASK) + 1u;
        out[n].size_bytes = size;
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
            if (saddr != 0u && saddr != addr) {
                out[n].pipe = pipe;
                out[n].primary = false;
                out[n].enabled = true;
                out[n].address = saddr;
                out[n].pitch_pixels = out[n - 1].pitch_pixels;
                out[n].height = out[n - 1].height;
                out[n].size_bytes = size;
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
    bool ok = true;

    pages = port_x86_64_paging_map_mmio(PORT_X86_64_GPU_VRAM_VIRT, bar0_base + test_offset,
                                        (k_size_t)GPU_AMD_VRAM_TEST_BYTES);
    if (pages == 0u) {
        kshell_printf("gpu vramtest: could not map BAR0+0x%x\n", (unsigned)test_offset);
        return 1;
    }

    p = (volatile k_u32_t *)(uintptr_t)PORT_X86_64_GPU_VRAM_VIRT;

    /* Read original contents. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        saved[i] = p[i];
    }

    kinfo("gpu vramtest: offset 0x%x size %u, saved first=0x%08x", (unsigned)test_offset,
          (unsigned)GPU_AMD_VRAM_TEST_BYTES, (unsigned)saved[0]);
    kshell_printf("gpu vramtest: offset 0x%x size %u\n", (unsigned)test_offset,
                  (unsigned)GPU_AMD_VRAM_TEST_BYTES);
    kshell_printf("  before: first=0x%08x last=0x%08x\n", (unsigned)saved[0],
                  (unsigned)saved[(GPU_AMD_VRAM_TEST_BYTES / 4u) - 1u]);

    /* Write pattern: 0xA5000000 | index. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        p[i] = 0xA5000000u ^ i;
    }

    /* Read back and verify. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        k_u32_t expect = 0xA5000000u ^ i;
        k_u32_t got = p[i];
        if (got != expect) {
            kshell_printf("  mismatch at +%u: got 0x%08x expect 0x%08x\n", (unsigned)(i * 4u),
                          (unsigned)got, (unsigned)expect);
            kerror("gpu vramtest: mismatch at +%u got 0x%08x expect 0x%08x", (unsigned)(i * 4u),
                   (unsigned)got, (unsigned)expect);
            ok = false;
            break;
        }
    }

    if (ok) {
        kshell_printf("  write/read: OK\n");
        kinfo("gpu vramtest: write/read ok");
    } else {
        kshell_printf("  write/read: FAILED, restoring original contents\n");
        kerror("gpu vramtest: write/read failed, restoring");
    }

    /* Restore original contents. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        p[i] = saved[i];
    }

    /* Verify restore. */
    for (i = 0; i < (GPU_AMD_VRAM_TEST_BYTES / 4u); i++) {
        k_u32_t got = p[i];
        if (got != saved[i]) {
            kshell_printf("  restore mismatch at +%u: got 0x%08x expect 0x%08x\n",
                          (unsigned)(i * 4u), (unsigned)got, (unsigned)saved[i]);
            kerror("gpu vramtest: restore mismatch at +%u", (unsigned)(i * 4u));
            return 1;
        }
    }

    kshell_printf("  restored: OK\n");
    kinfo("gpu vramtest: restored ok");
    return ok ? 0 : 1;
}

int port_x86_64_gpu_amd_vramtest(bool do_write)
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

    kshell_printf("gpu vramtest: scanout regions: %d\n", region_count);
    for (i = 0; i < region_count; i++) {
        kshell_printf("  pipe %u %s: addr 0x%x%08x size %u KiB (%ux%u)\n",
                      (unsigned)regions[i].pipe, regions[i].primary ? "primary" : "secondary",
                      (unsigned)(regions[i].address >> 32), (unsigned)(k_u32_t)regions[i].address,
                      (unsigned)(regions[i].size_bytes / 1024u), (unsigned)regions[i].pitch_pixels,
                      (unsigned)regions[i].height);
        kinfo("gpu vramtest: pipe %u %s addr 0x%x%08x size %u", (unsigned)regions[i].pipe,
              regions[i].primary ? "primary" : "secondary", (unsigned)(regions[i].address >> 32),
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
        kshell_printf("gpu vramtest: read-only check complete; pass --write to run write test\n");
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
