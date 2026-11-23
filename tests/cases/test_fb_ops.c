/*
 * Micro OS — the contract of an accelerated picture (kernel/fb_ops.h).
 *
 * The contract of 1.7 is three operations a device may have - a fill, a copy
 * and a flush - and the software path that answers when it has none. This
 * file holds the two promises of the header without a device:
 *
 *  * a slot with no operations (all NULL) draws exactly what the software
 *    path of 1.6 draws, pixel for pixel;
 *  * a device that offers the operations is the one that draws, and the
 *    rectangle it is given is the rectangle the caller asked for;
 *  * a rectangle that leaves the picture is cut to it and no byte of the
 *    picture after its last pixel is a pixel of the operation.
 *
 * The picture of the test is a small surface of the host and the device is a
 * stub that counts the calls and remembers their rectangles: the two of them
 * are the whole of what a machine with an engine and a machine without one
 * differ in.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/fb_ops.h"
#include "kernel/types.h"

/* The picture of the test: the screen of the port is 1280x800 and 8x8 is
 * enough for every edge of the contract. The pitch is wider than the picture,
 * which is a mode that pads a line, and the two lines of guard after it are
 * not pixels of it. */
#define TEST_WIDTH 8u
#define TEST_HEIGHT 6u
#define TEST_PITCH 10u
#define TEST_GUARD 2u
#define TEST_CELLS (TEST_PITCH * (TEST_HEIGHT + TEST_GUARD))

#define TEST_COLOR 0x1234u
#define TEST_OLD 0xFFFFu

static k_u16_t g_pixels[TEST_CELLS];

/* The stub of a device: what was asked of it, in the order it was asked. */
#define TEST_CALLS 8

struct test_device {
    int fills;
    int blits;
    int flushes;
    struct k_fb_rect rects[TEST_CALLS];
    k_u32_t colors[TEST_CALLS];
};

static struct test_device g_device;

static void device_reset(void)
{
    g_device.fills = 0;
    g_device.blits = 0;
    g_device.flushes = 0;
    memset(g_device.rects, 0, sizeof(g_device.rects));
    memset(g_device.colors, 0, sizeof(g_device.colors));
}

static int device_fill(void *context, const struct k_fb_rect *rect, k_u32_t pixel)
{
    struct test_device *device = (struct test_device *)context;

    if (device->fills < TEST_CALLS) {
        device->rects[device->fills] = *rect;
        device->colors[device->fills] = pixel;
    }

    device->fills++;

    return 0;
}

static int device_blit(void *context, const struct k_fb_rect *dst, const void *src,
                       k_u32_t src_pitch, k_u32_t src_x, k_u32_t src_y)
{
    struct test_device *device = (struct test_device *)context;

    (void)src;
    (void)src_pitch;
    (void)src_x;
    (void)src_y;

    if (device->blits < TEST_CALLS) {
        device->rects[device->blits] = *dst;
    }

    device->blits++;

    return 0;
}

static int device_flush(void *context, const struct k_fb_rect *rect)
{
    struct test_device *device = (struct test_device *)context;

    (void)rect;

    device->flushes++;

    return 0;
}

static void surface_clear(k_u16_t color)
{
    k_size_t index;

    for (index = 0; index < K_ARRAY_SIZE(g_pixels); index++) {
        g_pixels[index] = color;
    }
}

/* The picture of the test: the memory, its shape and the step of a line. */
static struct k_fb_surface test_surface(void)
{
    struct k_fb_surface surface;

    surface.base = g_pixels;
    surface.width = TEST_WIDTH;
    surface.height = TEST_HEIGHT;
    surface.pitch = TEST_PITCH;

    return surface;
}

static bool guard_is_clear(void)
{
    k_size_t index;

    /* The pixels of the last line of the picture are not the guard: the
     * guard is the two lines the pitch has on top of the six of the picture
     * and the padding of every line. */
    for (index = 0; index < TEST_CELLS; index++) {
        k_u32_t row = (k_u32_t)(index / TEST_PITCH);
        k_u32_t column = (k_u32_t)(index % TEST_PITCH);

        if (row >= TEST_HEIGHT && g_pixels[index] != TEST_OLD) {
            return false;
        }

        if (column >= TEST_WIDTH && g_pixels[index] != TEST_OLD) {
            return false;
        }
    }

    return true;
}

static void test_fb_ops_software(void)
{
    struct k_fb_surface surface = test_surface();
    struct k_fb_rect rect;
    /* Four lines of four cells: the blit below reads the rows 1 to 3. */
    k_u16_t source[4 * 4];
    k_size_t index;

    ktest_begin("fb_ops: a slot with no operation is the software path of 1.6");

    /* No device at all: the software path answers every call. */
    KT_CHECK_INT_EQ(k_fb_ops_register(NULL, NULL), 0);
    KT_CHECK_INT_EQ(k_fb_ops_device(NULL), NULL);

    surface_clear(TEST_OLD);

    rect.x = 2;
    rect.y = 1;
    rect.w = 3;
    rect.h = 2;
    KT_CHECK_INT_EQ(k_fb_fill(&surface, &rect, TEST_COLOR), 0);

    /* The rectangle and nothing else: the picture of a fill is the loop the
     * port has drawn since 1.6. */
    for (index = 0; index < TEST_CELLS; index++) {
        k_u32_t row = (k_u32_t)(index / TEST_PITCH);
        k_u32_t column = (k_u32_t)(index % TEST_PITCH);
        bool inside = row >= 1u && row < 3u && column >= 2u && column < 5u;

        KT_CHECK_INT_EQ(g_pixels[index], inside ? (k_u16_t)TEST_COLOR : TEST_OLD);
    }
    KT_CHECK_INT_EQ(guard_is_clear(), true);

    /* A blit of a block that is wider than the rectangle it copies: the
     * source is read at its offset and not at the start. */
    surface_clear(TEST_OLD);

    for (index = 0; index < K_ARRAY_SIZE(source); index++) {
        source[index] = (k_u16_t)(0x100u + index);
    }

    rect.x = 0;
    rect.y = 0;
    rect.w = 2;
    rect.h = 3;
    KT_CHECK_INT_EQ(k_fb_blit(&surface, &rect, source, 4, 1, 1), 0);

    KT_CHECK_INT_EQ(g_pixels[0 * TEST_PITCH + 0], source[1 * 4 + 1]);
    KT_CHECK_INT_EQ(g_pixels[0 * TEST_PITCH + 1], source[1 * 4 + 2]);
    KT_CHECK_INT_EQ(g_pixels[1 * TEST_PITCH + 0], source[2 * 4 + 1]);
    KT_CHECK_INT_EQ(g_pixels[2 * TEST_PITCH + 1], source[3 * 4 + 2]);
    KT_CHECK_INT_EQ(g_pixels[3 * TEST_PITCH + 0], TEST_OLD);

    /* The flush of the software picture is nothing to do and not an error. */
    rect.x = 0;
    rect.y = 0;
    rect.w = 1;
    rect.h = 1;
    KT_CHECK_INT_EQ(k_fb_flush(&surface, &rect), 0);
}

static void test_fb_ops_clip(void)
{
    struct k_fb_surface surface = test_surface();
    struct k_fb_rect rect;
    k_u16_t source[TEST_WIDTH * TEST_HEIGHT];

    ktest_begin("fb_ops: a rectangle that leaves the picture is cut to it");

    KT_CHECK_INT_EQ(k_fb_ops_register(NULL, NULL), 0);

    surface_clear(TEST_OLD);

    /* Half of the rectangle is left of the picture and above it: the part
     * that is inside is drawn. */
    rect.x = -3;
    rect.y = -2;
    rect.w = 5;
    rect.h = 4;
    KT_CHECK_INT_EQ(k_fb_fill(&surface, &rect, TEST_COLOR), 0);

    KT_CHECK_INT_EQ(g_pixels[0 * TEST_PITCH + 0], TEST_COLOR);
    KT_CHECK_INT_EQ(g_pixels[1 * TEST_PITCH + 1], TEST_COLOR);
    KT_CHECK_INT_EQ(g_pixels[1 * TEST_PITCH + 2], TEST_OLD);
    KT_CHECK_INT_EQ(g_pixels[2 * TEST_PITCH + 0], TEST_OLD);
    KT_CHECK_INT_EQ(guard_is_clear(), true);

    /* The other corner: the picture is eight by six and the rectangle is
     * wider and taller than what is left of it. */
    surface_clear(TEST_OLD);

    rect.x = TEST_WIDTH - 2;
    rect.y = TEST_HEIGHT - 1;
    rect.w = 4;
    rect.h = 4;
    KT_CHECK_INT_EQ(k_fb_fill(&surface, &rect, TEST_COLOR), 0);

    KT_CHECK_INT_EQ(g_pixels[TEST_HEIGHT - 1 == 5 ? 5 * TEST_PITCH + 6 : 0], TEST_COLOR);
    KT_CHECK_INT_EQ(g_pixels[5 * TEST_PITCH + 7], TEST_COLOR);
    KT_CHECK_INT_EQ(guard_is_clear(), true);

    /* A rectangle wholly outside the picture is no pixels and not an error. */
    surface_clear(TEST_OLD);

    rect.x = TEST_WIDTH + 4;
    rect.y = 0;
    rect.w = 2;
    rect.h = 2;
    KT_CHECK_INT_EQ(k_fb_fill(&surface, &rect, TEST_COLOR), 0);
    KT_CHECK_INT_EQ(g_pixels[0], TEST_OLD);
    KT_CHECK_INT_EQ(guard_is_clear(), true);

    /* A blit of a rectangle that leaves the right edge reads the source at
     * the cells of the part it draws. */
    memset(source, 0, sizeof(source));
    source[0] = 0xAAAAu;
    source[1] = 0xBBBBu;
    source[TEST_WIDTH] = 0xCCCCu;
    source[TEST_WIDTH + 1] = 0xDDDDu;

    surface_clear(TEST_OLD);
    rect.x = TEST_WIDTH - 1;
    rect.y = 0;
    rect.w = 2;
    rect.h = 2;
    KT_CHECK_INT_EQ(k_fb_blit(&surface, &rect, source, TEST_WIDTH, 0, 0), 0);
    KT_CHECK_INT_EQ(g_pixels[TEST_WIDTH - 1], 0xAAAAu);
    KT_CHECK_INT_EQ(g_pixels[TEST_PITCH + TEST_WIDTH - 1], 0xCCCCu);
    KT_CHECK_INT_EQ(g_pixels[TEST_WIDTH], TEST_OLD);
    KT_CHECK_INT_EQ(guard_is_clear(), true);
}

static void test_fb_ops_device(void)
{
    struct k_fb_surface surface = test_surface();
    struct k_fb_ops ops;
    struct k_fb_rect rect;
    k_u16_t source[TEST_WIDTH];
    void *context = NULL;
    k_size_t index;

    ktest_begin("fb_ops: a device with operations draws and the software path waits");

    device_reset();
    memset(&ops, 0, sizeof(ops));
    ops.fill = device_fill;
    ops.blit = device_blit;
    ops.flush = device_flush;

    KT_CHECK_INT_EQ(k_fb_ops_register(&ops, &g_device), 0);
    KT_CHECK_INT_EQ(k_fb_ops_device(&context), &ops);
    KT_CHECK_INT_EQ(context, &g_device);

    surface_clear(TEST_OLD);

    rect.x = 1;
    rect.y = 2;
    rect.w = 3;
    rect.h = 4;
    KT_CHECK_INT_EQ(k_fb_fill(&surface, &rect, 0x00FFu), 0);
    KT_CHECK_INT_EQ(g_device.fills, 1);
    KT_CHECK_INT_EQ(g_device.rects[0].x, 1);
    KT_CHECK_INT_EQ(g_device.rects[0].y, 2);
    KT_CHECK_INT_EQ(g_device.rects[0].w, 3);
    KT_CHECK_INT_EQ(g_device.rects[0].h, 4);
    KT_CHECK_INT_EQ(g_device.colors[0], 0x00FFu);

    /* The device drew nothing itself in this test, so the picture is the
     * one it was: the software path was not asked. */
    for (index = 0; index < TEST_CELLS; index++) {
        KT_CHECK_INT_EQ(g_pixels[index], TEST_OLD);
    }

    memset(source, 0, sizeof(source));
    KT_CHECK_INT_EQ(k_fb_blit(&surface, &rect, source, TEST_WIDTH, 0, 0), 0);
    KT_CHECK_INT_EQ(g_device.blits, 1);
    KT_CHECK_INT_EQ(g_device.rects[0].w, 3);

    KT_CHECK_INT_EQ(k_fb_flush(&surface, &rect), 0);
    KT_CHECK_INT_EQ(g_device.flushes, 1);

    /* A device that has one operation of the three answers that one and the
     * software path still draws the other two. */
    device_reset();
    ops.blit = NULL;
    surface_clear(TEST_OLD);
    rect.x = 0;
    rect.y = 0;
    rect.w = 2;
    rect.h = 1;
    KT_CHECK_INT_EQ(k_fb_fill(&surface, &rect, TEST_COLOR), 0);
    KT_CHECK_INT_EQ(g_device.fills, 1);
    KT_CHECK_INT_EQ(g_pixels[0], TEST_OLD);
    KT_CHECK_INT_EQ(k_fb_blit(&surface, &rect, source, TEST_WIDTH, 0, 0), 0);
    KT_CHECK_INT_EQ(g_device.blits, 0);
    KT_CHECK_INT_EQ(g_pixels[0], source[0]);

    /* And the slot goes back to nobody. */
    KT_CHECK_INT_EQ(k_fb_ops_register(NULL, NULL), 0);
    KT_CHECK_INT_EQ(k_fb_ops_device(NULL), NULL);
}

void test_fb_ops(void)
{
    test_fb_ops_software();
    test_fb_ops_clip();
    test_fb_ops_device();
}
