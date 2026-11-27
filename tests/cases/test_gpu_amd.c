/*
 * Micro OS — unit tests for the AMD GPU PCI ID table and GPU probe contract.
 *
 * The lookup is pure data: no PCI, no MMIO, no port dependencies. It guards
 * the "hello GPU" probe so a known card is identified and an unknown one is
 * not claimed. The vendor table is the stable surface Intel work will attach to.
 *
 * Facts from drivers/gpu/drm/amd/amdgpu/gmc_v8_0.c (MIT, Linux v6.12).
 * Code written here.
 */
#include "ktest.h"
#include "tests/tests.h"

#include "kernel/kmod.h"
#include "port_x86_64_gpu.h"
#include "port_x86_64_gpu_amd.h"
#include "port_x86_64_gpu_ids.h"

/* The vendor table in the header references the real AMD probe symbol. Host
 * unit tests do not link the x86 port probe, so provide a stub definition. */
enum gpu_probe_result port_x86_64_gpu_amd_probe(const struct port_x86_64_pci_device *pci,
                                                struct gpu_info *out)
{
    (void)pci;
    (void)out;
    return GPU_PROBE_NONE;
}

static void test_known_ids(void)
{
    const char *name;

    ktest_begin("gpu_amd: known Ellesmere 1002:67df");

    name = port_x86_64_gpu_amd_lookup(0x1002u, 0x67DFu);
    KT_CHECK(name != NULL);
    if (name != NULL) {
        KT_CHECK_STR_EQ(name, "AMD Radeon RX 470/480/570/570X/580/580X/590");
    }
}

static void test_other_polaris_ids(void)
{
    ktest_begin("gpu_amd: other Polaris IDs resolve");

    KT_CHECK(port_x86_64_gpu_amd_lookup(0x1002u, 0x67C7u) != NULL);
    KT_CHECK(port_x86_64_gpu_amd_lookup(0x1002u, 0x67EFu) != NULL);
    KT_CHECK(port_x86_64_gpu_amd_lookup(0x1002u, 0x67FFu) != NULL);
}

static void test_unknown(void)
{
    ktest_begin("gpu_amd: unknown device is not claimed");

    KT_CHECK(port_x86_64_gpu_amd_lookup(0x1002u, 0x1234u) == NULL);
    KT_CHECK(port_x86_64_gpu_amd_lookup(0x8086u, 0x67DFu) == NULL);
    KT_CHECK(port_x86_64_gpu_amd_lookup(0x0000u, 0x0000u) == NULL);
}

static void test_table_has_live_card(void)
{
    k_size_t i;
    bool found = false;

    ktest_begin("gpu_amd: table contains live recon card 1002:67df");

    for (i = 0; i < PORT_X86_64_GPU_AMD_ID_COUNT; i++) {
        if (PORT_X86_64_GPU_AMD_IDS[i].vendor == 0x1002u &&
            PORT_X86_64_GPU_AMD_IDS[i].device == 0x67DFu) {
            found = true;
            break;
        }
    }

    KT_CHECK(found);
    KT_CHECK(PORT_X86_64_GPU_AMD_ID_COUNT >= 1u);
}

static void test_vendor_table(void)
{
    ktest_begin("gpu: vendor probe table has AMD then Intel weak slot");

    KT_CHECK_INT_EQ((long long)PORT_X86_64_GPU_PROBE_COUNT, 2);
    KT_CHECK_INT_EQ(PORT_X86_64_GPU_PROBES[0].vendor, 0x1002u);
    KT_CHECK_INT_EQ(PORT_X86_64_GPU_PROBES[1].vendor, 0x8086u);
    KT_CHECK(PORT_X86_64_GPU_PROBES[0].probe == port_x86_64_gpu_amd_probe);
    /* Intel probe is a weak symbol: linked in the merged build, absent when
     * gpu_intel.c is not compiled in. The dispatcher must still have the slot. */
    KT_CHECK(PORT_X86_64_GPU_PROBES[1].probe == gpu_intel_probe);
}

static void test_kmod_state_names(void)
{
    ktest_begin("gpu: KMOD_UNKNOWN is distinct from KMOD_SKIP");

    KT_CHECK_STR_EQ(kmod_state_name(KMOD_UNKNOWN), "unknown");
    KT_CHECK_STR_EQ(kmod_state_name(KMOD_SKIP), "skip");
    KT_CHECK(KMOD_UNKNOWN != KMOD_SKIP);
}

static void test_mc_memsize_decode(void)
{
    ktest_begin("gpu_amd: CONFIG_MEMSIZE decode clamps garbage upper bits");

    KT_CHECK_INT_EQ((long long)gpu_amd_decode_memsize_mb(8192u), 8192);
    KT_CHECK_INT_EQ((long long)gpu_amd_decode_memsize_mb(4096u), 4096);
    /* Linux gmc_v8_0.c: if upper 16 bits set and lower 16 nonzero, clamp. */
    KT_CHECK_INT_EQ((long long)gpu_amd_decode_memsize_mb(0xDEAD2000u), 0x2000);
    KT_CHECK_INT_EQ((long long)gpu_amd_decode_memsize_mb(0xFFFF0000u), 0);
    KT_CHECK_INT_EQ((long long)gpu_amd_decode_memsize_mb(0u), 0);
}

static void test_mc_chmap_decode(void)
{
    ktest_begin("gpu_amd: MC_SHARED_CHMAP NOOFCHAN decode");

    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(0u << 12), 1);
    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(1u << 12), 2);
    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(2u << 12), 4);
    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(3u << 12), 8);
    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(4u << 12), 3);
    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(5u << 12), 6);
    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(6u << 12), 10);
    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(7u << 12), 12);
    KT_CHECK_INT_EQ(gpu_amd_decode_noofchan(8u << 12), 16);
}

static void test_mc_ramcfg_decode(void)
{
    ktest_begin("gpu_amd: MC_ARB_RAMCFG CHANSIZE decode");

    KT_CHECK_INT_EQ(gpu_amd_decode_chansize(0u), 32);
    KT_CHECK_INT_EQ(gpu_amd_decode_chansize(1u << 8), 64);
    KT_CHECK_INT_EQ(gpu_amd_decode_chansize(0xFFFFFEFFu), 32);
}

static void test_scanout_size_decode(void)
{
    ktest_begin("gpu_amd: scanout size decode is conservative");

    /* 1920 * 1080 * 4 = 8294400, clamped to >= 1 MiB */
    KT_CHECK_INT_EQ((long long)gpu_amd_scanout_size(1920u, 1079u), 1920ll * 1080ll * 4ll);
    /* Zero pitch -> 0; small surface clamps up to 1 MiB */
    KT_CHECK_INT_EQ((long long)gpu_amd_scanout_size(0u, 1079u), 0);
    KT_CHECK_INT_EQ((long long)gpu_amd_scanout_size(64u, 0u), 1024ll * 1024ll);
    /* Clamp to 256 MiB */
    KT_CHECK_INT_EQ((long long)gpu_amd_scanout_size(0x7FFFu, 0x3FFFu), 256ll * 1024ll * 1024ll);
}

static void test_scanout_overlap(void)
{
    struct gpu_amd_scanout_region r;

    ktest_begin("gpu_amd: vram test refuses overlap with scanout");

    memset(&r, 0, sizeof(r));
    r.pipe = 0;
    r.primary = true;
    r.enabled = true;
    r.address = 8ull * 1024ull * 1024ull * 1024ull - 4ull * 1024ull * 1024ull;
    r.size_bytes = 16ull * 1024ull * 1024ull;

    KT_CHECK(gpu_amd_ranges_overlap(r.address, r.size_bytes, r.address + 0x1000u, 4096u));
    KT_CHECK(!gpu_amd_ranges_overlap(r.address, r.size_bytes, r.address - 4096u - 1ull, 4096u));

    /* choose_test_offset must refuse when last-4MiB candidate overlaps scanout. */
    {
        k_u64_t vram = 8ull * 1024ull * 1024ull * 1024ull;
        k_u64_t off = gpu_amd_choose_test_offset(vram, &r, 1);
        /* It should either find a non-overlapping lower candidate or return 0. */
        if (off != 0u) {
            KT_CHECK(!gpu_amd_scanout_overlaps(off, GPU_AMD_VRAM_TEST_BYTES, &r, 1));
            KT_CHECK(off >= GPU_AMD_VRAM_TEST_MIN_OFFSET);
        }
    }
}

static void test_scanout_overlap_refuses(void)
{
    struct gpu_amd_scanout_region regions[2];
    k_u64_t vram = 256ull * 1024ull * 1024ull;
    k_u64_t off;

    ktest_begin("gpu_amd: vram test refuses when all candidates overlap scanout");

    memset(regions, 0, sizeof(regions));
    /* Cover the whole usable area above 16MiB with scanout regions. */
    regions[0].enabled = true;
    regions[0].address = 16ull * 1024ull * 1024ull;
    regions[0].size_bytes = 240ull * 1024ull * 1024ull;

    off = gpu_amd_choose_test_offset(vram, regions, 1);
    KT_CHECK_INT_EQ((long long)off, 0);
}

static void test_vram_mgr_basic(void)
{
    struct gpu_amd_vram_mgr mgr;
    struct gpu_amd_scanout_region scan[1];
    k_u32_t id = 0u;
    k_u64_t off = 0u;
    int rc;

    ktest_begin("gpu_amd: vram mgr alloc/free basic");

    memset(scan, 0, sizeof(scan));
    rc = gpu_amd_vram_mgr_init(&mgr, 256ull * 1024ull * 1024ull, scan, 0);
    KT_CHECK_INT_EQ(rc, GPU_AMD_VRAM_OK);
    KT_CHECK(mgr.ready);
    KT_CHECK_INT_EQ(mgr.free_count, 1);
    KT_CHECK_INT_EQ((long long)mgr.free_extents[0].offset, (long long)GPU_AMD_VRAM_RESERVED_END);

    rc = gpu_amd_vram_alloc(&mgr, 1024ull * 1024ull, &id, &off);
    KT_CHECK_INT_EQ(rc, GPU_AMD_VRAM_OK);
    KT_CHECK(id != 0u);
    KT_CHECK_INT_EQ((long long)off, (long long)GPU_AMD_VRAM_RESERVED_END);
    KT_CHECK(gpu_amd_vram_find(&mgr, id) != NULL);

    rc = gpu_amd_vram_free(&mgr, id);
    KT_CHECK_INT_EQ(rc, GPU_AMD_VRAM_OK);
    KT_CHECK(gpu_amd_vram_find(&mgr, id) == NULL);
    KT_CHECK_INT_EQ(mgr.free_count, 1);
}

static void test_vram_mgr_coalesce(void)
{
    struct gpu_amd_vram_mgr mgr;
    k_u32_t a = 0u, b = 0u, c = 0u;
    k_u64_t off = 0u;
    int rc;

    ktest_begin("gpu_amd: vram mgr coalesces neighbors on free");

    rc = gpu_amd_vram_mgr_init(&mgr, 64ull * 1024ull * 1024ull, NULL, 0);
    KT_CHECK_INT_EQ(rc, GPU_AMD_VRAM_OK);

    KT_CHECK_INT_EQ(gpu_amd_vram_alloc(&mgr, 64ull * 1024u, &a, &off), GPU_AMD_VRAM_OK);
    KT_CHECK_INT_EQ(gpu_amd_vram_alloc(&mgr, 64ull * 1024u, &b, &off), GPU_AMD_VRAM_OK);
    KT_CHECK_INT_EQ(gpu_amd_vram_alloc(&mgr, 64ull * 1024u, &c, &off), GPU_AMD_VRAM_OK);

    KT_CHECK_INT_EQ(gpu_amd_vram_free(&mgr, a), GPU_AMD_VRAM_OK);
    KT_CHECK_INT_EQ(gpu_amd_vram_free(&mgr, c), GPU_AMD_VRAM_OK);
    /* Hole before b and merged hole after b+remainder = 2 extents. */
    KT_CHECK_INT_EQ(mgr.free_count, 2);

    KT_CHECK_INT_EQ(gpu_amd_vram_free(&mgr, b), GPU_AMD_VRAM_OK);
    KT_CHECK_INT_EQ(mgr.free_count, 1);
    KT_CHECK_INT_EQ((long long)mgr.free_extents[0].offset, (long long)GPU_AMD_VRAM_RESERVED_END);
}

static void test_vram_mgr_double_free(void)
{
    struct gpu_amd_vram_mgr mgr;
    k_u32_t id = 0u;
    k_u64_t off = 0u;

    ktest_begin("gpu_amd: vram mgr rejects double free and bad id");

    KT_CHECK_INT_EQ(gpu_amd_vram_mgr_init(&mgr, 64ull * 1024ull * 1024ull, NULL, 0),
                    GPU_AMD_VRAM_OK);
    KT_CHECK_INT_EQ(gpu_amd_vram_alloc(&mgr, 64ull * 1024u, &id, &off), GPU_AMD_VRAM_OK);
    KT_CHECK_INT_EQ(gpu_amd_vram_free(&mgr, id), GPU_AMD_VRAM_OK);
    KT_CHECK_INT_EQ(gpu_amd_vram_free(&mgr, id), GPU_AMD_VRAM_ERR_BAD_ID);
    KT_CHECK_INT_EQ(gpu_amd_vram_free(&mgr, 9999u), GPU_AMD_VRAM_ERR_BAD_ID);
    KT_CHECK_INT_EQ(gpu_amd_vram_free(&mgr, 0u), GPU_AMD_VRAM_ERR_BAD_ID);
}

static void test_vram_mgr_exhaustion(void)
{
    struct gpu_amd_vram_mgr mgr;
    k_u32_t id = 0u;
    k_u64_t off = 0u;
    int rc;

    ktest_begin("gpu_amd: vram mgr reports no space / range errors");

    KT_CHECK_INT_EQ(gpu_amd_vram_mgr_init(&mgr, 32ull * 1024ull * 1024ull, NULL, 0),
                    GPU_AMD_VRAM_OK);
    rc = gpu_amd_vram_alloc(&mgr, 32ull * 1024ull * 1024ull, &id, &off);
    KT_CHECK_INT_EQ(rc, GPU_AMD_VRAM_ERR_NO_SPACE);

    rc = gpu_amd_vram_alloc(&mgr, 0u, &id, &off);
    KT_CHECK_INT_EQ(rc, GPU_AMD_VRAM_ERR_RANGE);

    rc = gpu_amd_vram_alloc(&mgr, 1ull * 1024ull * 1024ull * 1024ull * 16ull, &id, &off);
    KT_CHECK_INT_EQ(rc, GPU_AMD_VRAM_ERR_RANGE);
}

static void test_vram_mgr_scanout_reserved(void)
{
    struct gpu_amd_vram_mgr mgr;
    struct gpu_amd_scanout_region scan[1];
    k_u32_t id = 0u;
    k_u64_t off = 0u;

    ktest_begin("gpu_amd: vram mgr excludes scanout and first 16 MiB");

    memset(scan, 0, sizeof(scan));
    scan[0].enabled = true;
    scan[0].pipe = 0;
    scan[0].primary = true;
    scan[0].address = 32ull * 1024ull * 1024ull;
    scan[0].size_bytes = 16ull * 1024ull * 1024ull;

    KT_CHECK_INT_EQ(gpu_amd_vram_mgr_init(&mgr, 256ull * 1024ull * 1024ull, scan, 1),
                    GPU_AMD_VRAM_OK);

    /* First allocation should be below scanout, at 16 MiB. */
    KT_CHECK_INT_EQ(gpu_amd_vram_alloc(&mgr, 1024ull * 1024ull, &id, &off), GPU_AMD_VRAM_OK);
    KT_CHECK_INT_EQ((long long)off, (long long)GPU_AMD_VRAM_RESERVED_END);
    KT_CHECK(!gpu_amd_scanout_overlaps(off, 1024ull * 1024ull, scan, 1));

    /* A fake allocation overlapping scanout must be rejected by overlap check. */
    KT_CHECK(gpu_amd_vram_reserved_overlap(&mgr, 32ull * 1024ull * 1024ull, 4096u));
    KT_CHECK(gpu_amd_vram_reserved_overlap(&mgr, 0u, 4096u));
    KT_CHECK(!gpu_amd_vram_reserved_overlap(&mgr, 64ull * 1024ull * 1024ull, 4096u));
}

static void test_vram_mgr_fragmentation(void)
{
    struct gpu_amd_vram_mgr mgr;
    k_u32_t ids[8];
    k_u64_t off = 0u;
    int i;
    int rc;
    k_u32_t big = 0u;

    ktest_begin("gpu_amd: vram mgr handles fragmentation");

    /* Pool exactly 8 blocks: no remainder after filling. */
    KT_CHECK_INT_EQ(gpu_amd_vram_mgr_init(
                        &mgr, GPU_AMD_VRAM_RESERVED_END + 8ull * GPU_AMD_VRAM_BLOCK_SIZE, NULL, 0),
                    GPU_AMD_VRAM_OK);
    for (i = 0; i < 8; i++) {
        ids[i] = 0u;
        KT_CHECK_INT_EQ(gpu_amd_vram_alloc(&mgr, 64ull * 1024u, &ids[i], &off), GPU_AMD_VRAM_OK);
    }
    /* Free every other block to fragment. */
    for (i = 0; i < 8; i += 2) {
        KT_CHECK_INT_EQ(gpu_amd_vram_free(&mgr, ids[i]), GPU_AMD_VRAM_OK);
    }
    /* A large allocation should fail (holes are 64K each). */
    rc = gpu_amd_vram_alloc(&mgr, 128ull * 1024u, &big, &off);
    KT_CHECK_INT_EQ(rc, GPU_AMD_VRAM_ERR_NO_SPACE);
    /* Small allocations fit into holes. */
    KT_CHECK_INT_EQ(gpu_amd_vram_alloc(&mgr, 64ull * 1024u, &big, &off), GPU_AMD_VRAM_OK);
}

void test_gpu_amd(void)
{
    test_known_ids();
    test_other_polaris_ids();
    test_unknown();
    test_table_has_live_card();
    test_vendor_table();
    test_kmod_state_names();
    test_mc_memsize_decode();
    test_mc_chmap_decode();
    test_mc_ramcfg_decode();
    test_scanout_size_decode();
    test_scanout_overlap();
    test_scanout_overlap_refuses();
    test_vram_mgr_basic();
    test_vram_mgr_coalesce();
    test_vram_mgr_double_free();
    test_vram_mgr_exhaustion();
    test_vram_mgr_scanout_reserved();
    test_vram_mgr_fragmentation();
}
