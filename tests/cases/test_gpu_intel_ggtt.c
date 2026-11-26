/*
 * Micro OS — synthetic global page tables of the Intel display driver.
 *
 * Facts from drivers/gpu/drm/i915/gt/intel_gtt.h and
 * drivers/gpu/drm/i915/display/skl_universal_plane.c (GPL-2.0, Linux v6.12);
 * code written here for Micro OS (MIT).
 */
#include "ktest.h"
#include "tests/tests.h"

#include "gpu_intel_ggtt.h"

#define TEST_GGTT_ENTRIES 4096u

static k_u64_t g_entries[TEST_GGTT_ENTRIES];

static void test_x_tiled_laptop_frame(void)
{
    struct gpu_intel_fb fb;
    k_u32_t control =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_XRGB_8888 << 24) | (GPU_INTEL_TILE_X << 10);
    k_u32_t i;
    enum gpu_intel_ggtt_result rc;

    ktest_begin("gpu-intel: a synthetic 1920x1080 X-tiled frame resolves through GGTT");

    for (i = 0u; i < TEST_GGTT_ENTRIES; i++) {
        g_entries[i] =
            0x123450000000ULL + (k_u64_t)i * GPU_INTEL_GTT_PAGE_SIZE + GPU_INTEL_GTT_PRESENT;
    }

    rc = gpu_intel_ggtt_decode_fb(g_entries, TEST_GGTT_ENTRIES, 0x00200000u, 15u, control, 1920u,
                                  1080u, &fb);

    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    KT_CHECK(fb.surface == 0x00200000u);
    KT_CHECK(fb.phys == 0x123450200000ULL);
    KT_CHECK_INT_EQ(fb.width, 1920);
    KT_CHECK_INT_EQ(fb.height, 1080);
    KT_CHECK_INT_EQ(fb.pitch, 7680);
    KT_CHECK_INT_EQ(fb.bytes, 8294400);
    KT_CHECK_INT_EQ(fb.pte_start, 512);
    KT_CHECK_INT_EQ(fb.pte_count, 2025);
    KT_CHECK_INT_EQ(fb.bpp, 4);
    KT_CHECK(fb.contiguous);
}

static void test_linear_rgb565_frame(void)
{
    struct gpu_intel_fb fb;
    k_u32_t control =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_RGB_565 << 24) | (GPU_INTEL_TILE_LINEAR << 10);
    k_u32_t i;
    enum gpu_intel_ggtt_result rc;

    ktest_begin("gpu-intel: a linear RGB565 stride is counted in 64-byte units");

    for (i = 0u; i < TEST_GGTT_ENTRIES; i++) {
        g_entries[i] =
            0x000000100000ULL + (k_u64_t)i * GPU_INTEL_GTT_PAGE_SIZE + GPU_INTEL_GTT_PRESENT;
    }

    rc = gpu_intel_ggtt_decode_fb(g_entries, TEST_GGTT_ENTRIES, 0x00100000u, 20u, control, 640u,
                                  480u, &fb);

    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    KT_CHECK(fb.phys == 0x000000200000ULL);
    KT_CHECK_INT_EQ(fb.pitch, 1280);
    KT_CHECK_INT_EQ(fb.bytes, 614400);
    KT_CHECK_INT_EQ(fb.pte_count, 150);
    KT_CHECK_INT_EQ(fb.bpp, 2);
    KT_CHECK(fb.contiguous);
}

static void test_yf_padding_and_fragmentation(void)
{
    struct gpu_intel_fb fb;
    k_u32_t control =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_XRGB_8888 << 24) | (GPU_INTEL_TILE_YF << 10);
    k_u32_t i;
    enum gpu_intel_ggtt_result rc;

    ktest_begin("gpu-intel: Yf scanout includes tile padding and reports fragments");

    for (i = 0u; i < TEST_GGTT_ENTRIES; i++) {
        k_u64_t physical = 0x000000200000ULL + (k_u64_t)i * GPU_INTEL_GTT_PAGE_SIZE;
        if (i == 600u) {
            physical += GPU_INTEL_GTT_PAGE_SIZE;
        }
        g_entries[i] = physical | GPU_INTEL_GTT_PRESENT;
    }

    rc = gpu_intel_ggtt_decode_fb(g_entries, TEST_GGTT_ENTRIES, 0x00200000u, 60u, control, 1920u,
                                  1080u, &fb);

    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    KT_CHECK_INT_EQ(fb.pitch, 7680);
    KT_CHECK_INT_EQ(fb.bytes, 8355840);
    KT_CHECK_INT_EQ(fb.pte_count, 2040);
    KT_CHECK(!fb.contiguous);
}

static void test_rejections(void)
{
    struct gpu_intel_fb fb;
    k_u32_t control =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_XRGB_8888 << 24) | (GPU_INTEL_TILE_X << 10);
    enum gpu_intel_ggtt_result rc;

    ktest_begin("gpu-intel: a missing or absent page-table entry is not a frame");

    g_entries[0] = 0x000000100000ULL | GPU_INTEL_GTT_PRESENT;
    rc = gpu_intel_ggtt_decode_fb(g_entries, 1u, 0u, 15u, control, 1920u, 1080u, &fb);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_UNMAPPED);

    g_entries[0] = 0x000000100000ULL;
    rc = gpu_intel_ggtt_decode_fb(g_entries, 1u, 0u, 15u, control, 1920u, 1080u, &fb);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_UNMAPPED);

    rc = gpu_intel_ggtt_decode_fb(g_entries, TEST_GGTT_ENTRIES, 0u, 15u,
                                  control & ~GPU_INTEL_PLANE_ENABLE, 1920u, 1080u, &fb);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_INVALID);
}

void test_gpu_intel_ggtt(void)
{
    test_x_tiled_laptop_frame();
    test_linear_rgb565_frame();
    test_yf_padding_and_fragmentation();
    test_rejections();
}
