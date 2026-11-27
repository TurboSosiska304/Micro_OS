/*
 * Micro OS — synthetic global page tables of the Intel display driver.
 *
 * Facts from drivers/gpu/drm/i915/gt/intel_gtt.h,
 * drivers/gpu/drm/i915/gt/intel_ggtt.c,
 * drivers/gpu/drm/i915/display/intel_fb.c and
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

static void test_free_run_and_exclusions(void)
{
    struct gpu_intel_gtt_run run;
    struct gpu_intel_gtt_range excluded[1];
    k_u32_t i;
    enum gpu_intel_ggtt_result rc;

    ktest_begin("gpu-intel: a free GGTT run has scratch guards and avoids live surfaces");

    for (i = 0u; i < TEST_GGTT_ENTRIES; i++) {
        g_entries[i] = 0x000000008000ULL | GPU_INTEL_GTT_PRESENT;
    }
    excluded[0] = (struct gpu_intel_gtt_range){1u, 2u};
    rc = gpu_intel_ggtt_find_free(g_entries, TEST_GGTT_ENTRIES, 2u, excluded, 1u, &run);

    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    KT_CHECK_INT_EQ(run.start, 3);
    KT_CHECK_INT_EQ(run.count, 2);
    KT_CHECK(run.original == (0x000000008000ULL | GPU_INTEL_GTT_PRESENT));
}

static void test_free_run_requires_guards(void)
{
    struct gpu_intel_gtt_run run;
    enum gpu_intel_ggtt_result rc;

    ktest_begin("gpu-intel: an unguarded scratch run is not a free GGTT run");

    g_entries[0] = 0x000000009000ULL | GPU_INTEL_GTT_PRESENT;
    g_entries[1] = 0x000000008000ULL | GPU_INTEL_GTT_PRESENT;
    g_entries[2] = 0x000000008000ULL | GPU_INTEL_GTT_PRESENT;
    g_entries[3] = 0x000000009000ULL | GPU_INTEL_GTT_PRESENT;
    rc = gpu_intel_ggtt_find_free(g_entries, 4u, 1u, NULL, 0u, &run);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_NO_FREE);

    g_entries[1] = 0u;
    g_entries[2] = 0u;
    rc = gpu_intel_ggtt_find_free(g_entries, 4u, 1u, NULL, 0u, &run);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_NO_FREE);
}

static void test_borrow_and_restore(void)
{
    struct gpu_intel_gtt_run run;
    k_u64_t snapshot[32];
    k_u32_t i;
    enum gpu_intel_ggtt_result rc;
    k_u64_t saved = 0u;

    ktest_begin("gpu-intel: borrowing GGTT pages changes only the chosen run");

    for (i = 0u; i < 32u; i++) {
        g_entries[i] = 0x000000008000ULL | GPU_INTEL_GTT_PRESENT;
        snapshot[i] = g_entries[i];
    }
    rc = gpu_intel_ggtt_find_free(g_entries, 32u, 2u, NULL, 0u, &run);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    if (rc != GPU_INTEL_GGTT_OK) {
        return;
    }

    rc = gpu_intel_ggtt_borrow(g_entries, 32u, &run, 0x000000100000ULL, &saved);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    KT_CHECK(g_entries[run.start] == (0x000000100000ULL | GPU_INTEL_GTT_PRESENT));
    KT_CHECK(g_entries[run.start + 1u] == (0x000000101000ULL | GPU_INTEL_GTT_PRESENT));
    KT_CHECK(g_entries[run.start - 1u] == snapshot[run.start - 1u]);
    KT_CHECK(g_entries[run.start + 2u] == snapshot[run.start + 2u]);

    rc = gpu_intel_ggtt_restore(g_entries, 32u, &run, saved);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    for (i = 0u; i < 32u; i++) {
        KT_CHECK(g_entries[i] == snapshot[i]);
    }
}

static void test_install_fragmented_pages(void)
{
    struct gpu_intel_gtt_run run;
    k_u64_t physical[3];
    k_u64_t snapshot[32];
    k_u64_t saved = 0u;
    k_u32_t i;
    enum gpu_intel_ggtt_result rc;

    ktest_begin("gpu-intel: a RAM surface may install non-contiguous GGTT pages");

    for (i = 0u; i < 32u; i++) {
        g_entries[i] = 0x000000008000ULL | GPU_INTEL_GTT_PRESENT;
        snapshot[i] = g_entries[i];
    }
    rc = gpu_intel_ggtt_find_free(g_entries, 32u, 3u, NULL, 0u, &run);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    if (rc != GPU_INTEL_GGTT_OK) {
        return;
    }

    physical[0] = 0x000000100000ULL;
    physical[1] = 0x000000180000ULL;
    physical[2] = 0x000000120000ULL;
    rc = gpu_intel_ggtt_install(g_entries, 32u, &run, physical, &saved);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    for (i = 0u; i < run.count; i++) {
        KT_CHECK(g_entries[run.start + i] == (physical[i] | GPU_INTEL_GTT_PRESENT));
    }
    KT_CHECK(g_entries[run.start - 1u] == snapshot[run.start - 1u]);
    KT_CHECK(g_entries[run.start + run.count] == snapshot[run.start + run.count]);

    rc = gpu_intel_ggtt_restore(g_entries, 32u, &run, saved);
    KT_CHECK_INT_EQ(rc, GPU_INTEL_GGTT_OK);
    for (i = 0u; i < 32u; i++) {
        KT_CHECK(g_entries[i] == snapshot[i]);
    }
}

static void test_unmapped_run_is_free(void)
{
    struct gpu_intel_gtt_run run;
    k_u64_t saved = 77u;
    k_u32_t i;

    ktest_begin("gpu-intel: unmapped GGTT pages can be borrowed with unmapped guards");

    for (i = 0u; i < 32u; i++) {
        g_entries[i] = 0u;
    }
    KT_CHECK_INT_EQ(gpu_intel_ggtt_find_free(g_entries, 32u, 2u, NULL, 0u, &run),
                    GPU_INTEL_GGTT_OK);
    KT_CHECK_INT_EQ(run.start, 1);
    KT_CHECK_INT_EQ(run.count, 2);
    KT_CHECK(run.original == 0u);

    KT_CHECK_INT_EQ(gpu_intel_ggtt_borrow(g_entries, 32u, &run, 0x000000100000ULL, &saved),
                    GPU_INTEL_GGTT_OK);
    KT_CHECK(saved == 0u);
    KT_CHECK(g_entries[0] == 0u);
    KT_CHECK(g_entries[1] == (0x000000100000ULL | GPU_INTEL_GTT_PRESENT));
    KT_CHECK(g_entries[2] == (0x000000101000ULL | GPU_INTEL_GTT_PRESENT));
    KT_CHECK(g_entries[3] == 0u);
    KT_CHECK_INT_EQ(gpu_intel_ggtt_restore(g_entries, 32u, &run, saved), GPU_INTEL_GGTT_OK);
    for (i = 0u; i < 32u; i++) {
        KT_CHECK(g_entries[i] == 0u);
    }
}

static void test_identity_run_needs_no_write(void)
{
    struct gpu_intel_gtt_run run;
    struct gpu_intel_gtt_range excluded[1];
    k_u64_t physical[3];
    k_u32_t i;

    ktest_begin("gpu-intel: an identity GGTT run maps RAM without a PTE write");

    for (i = 0u; i < 32u; i++) {
        g_entries[i] = 0u;
    }
    for (i = 0u; i < 3u; i++) {
        physical[i] = (k_u64_t)(20u + i) * GPU_INTEL_GTT_PAGE_SIZE;
        g_entries[20u + i] = physical[i] | GPU_INTEL_GTT_PRESENT;
    }
    KT_CHECK_INT_EQ(gpu_intel_ggtt_find_identity(g_entries, 32u, physical, 3u, NULL, 0u, &run),
                    GPU_INTEL_GGTT_OK);
    KT_CHECK_INT_EQ(run.start, 20);
    KT_CHECK_INT_EQ(run.count, 3);

    excluded[0] = (struct gpu_intel_gtt_range){21u, 1u};
    KT_CHECK_INT_EQ(gpu_intel_ggtt_find_identity(g_entries, 32u, physical, 3u, excluded, 1u, &run),
                    GPU_INTEL_GGTT_NO_FREE);
}

static void test_stats_census(void)
{
    struct gpu_intel_ggtt_stats stats;
    k_u32_t i;

    ktest_begin("gpu-intel: the GGTT census reports present, unmapped and identity pages");

    for (i = 0u; i < 32u; i++) {
        g_entries[i] = 0u;
    }
    for (i = 1u; i < 5u; i++) {
        g_entries[i] = (k_u64_t)i * GPU_INTEL_GTT_PAGE_SIZE | GPU_INTEL_GTT_PRESENT;
    }
    for (i = 9u; i < 12u; i++) {
        g_entries[i] = 0x000000008000ULL | GPU_INTEL_GTT_PRESENT;
    }
    gpu_intel_ggtt_stats(g_entries, 32u, &stats);
    KT_CHECK_INT_EQ(stats.entries, 32);
    KT_CHECK_INT_EQ(stats.present, 7);
    KT_CHECK_INT_EQ(stats.unmapped, 25);
    KT_CHECK_INT_EQ(stats.identity, 4);
    KT_CHECK_INT_EQ(stats.scratch_ranges, 5);
    KT_CHECK_INT_EQ(stats.longest_scratch, 3);
    KT_CHECK_INT_EQ(stats.longest_unmapped, 20);
    KT_CHECK_INT_EQ(stats.common_count, 3);
}

static void test_plane_ctl_decode(void)
{
    k_u32_t xrgb_x_tiled =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_XRGB_8888 << 24) | (GPU_INTEL_TILE_X << 10);
    k_u32_t rgb565_linear =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_RGB_565 << 24) | (GPU_INTEL_TILE_LINEAR << 10);
    struct gpu_intel_plane_ctl ctl;

    ktest_begin("gpu-intel: PLANE_CTL decode reports format, tiling and bpp");

    ctl = gpu_intel_plane_ctl_decode(xrgb_x_tiled);
    KT_CHECK(ctl.valid);
    KT_CHECK_INT_EQ(ctl.format, GPU_INTEL_FORMAT_XRGB_8888);
    KT_CHECK_INT_EQ(ctl.tiling, GPU_INTEL_TILE_X);
    KT_CHECK_INT_EQ(ctl.bpp, 4u);
    KT_CHECK_INT_EQ(ctl.stride_unit, 512u);
    KT_CHECK_STR_EQ(gpu_intel_format_name(ctl.format), "XRGB8888");
    KT_CHECK_STR_EQ(gpu_intel_tiling_name(ctl.tiling), "X-tiled");

    ctl = gpu_intel_plane_ctl_decode(rgb565_linear);
    KT_CHECK(ctl.valid);
    KT_CHECK_INT_EQ(ctl.format, GPU_INTEL_FORMAT_RGB_565);
    KT_CHECK_INT_EQ(ctl.tiling, GPU_INTEL_TILE_LINEAR);
    KT_CHECK_INT_EQ(ctl.bpp, 2u);
    KT_CHECK_INT_EQ(ctl.stride_unit, 64u);
}

static void test_plane_ctl_present_ok(void)
{
    k_u32_t xrgb_x_tiled =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_XRGB_8888 << 24) | (GPU_INTEL_TILE_X << 10);
    k_u32_t xrgb_linear =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_XRGB_8888 << 24) | (GPU_INTEL_TILE_LINEAR << 10);
    k_u32_t rgb565_linear =
        GPU_INTEL_PLANE_ENABLE | (GPU_INTEL_FORMAT_RGB_565 << 24) | (GPU_INTEL_TILE_LINEAR << 10);

    ktest_begin("gpu-intel: page flip refuses tiled planes and accepts linear RGB565/XRGB8888");

    KT_CHECK(!gpu_intel_plane_ctl_present_ok(xrgb_x_tiled));
    KT_CHECK(gpu_intel_plane_ctl_present_ok(xrgb_linear));
    KT_CHECK(gpu_intel_plane_ctl_present_ok(rgb565_linear));
    KT_CHECK(!gpu_intel_plane_ctl_present_ok(0u));
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
    test_free_run_and_exclusions();
    test_free_run_requires_guards();
    test_borrow_and_restore();
    test_install_fragmented_pages();
    test_unmapped_run_is_free();
    test_identity_run_needs_no_write();
    test_stats_census();
    test_plane_ctl_decode();
    test_plane_ctl_present_ok();
    test_rejections();
}
