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
}
