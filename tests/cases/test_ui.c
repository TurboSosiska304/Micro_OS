/*
 * Micro OS — the contract of the two devices of the interface (ADR 0028).
 *
 * The shape of a `/dev/fb0` write and the names of the keys of
 * `/dev/input0` are portable: every machine that offers the devices speaks
 * them, so the rules live in `components/kernel` and are tested here, on the
 * host, without a screen or a keyboard. The devices of the port are a layer
 * over these two contracts.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stddef.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/fb.h"
#include "kernel/input.h"

#define FB_TEST_WIDTH 1280u
#define FB_TEST_HEIGHT 800u

static void info_default(struct fb_info *info)
{
    memset(info, 0, sizeof(*info));
    info->width = FB_TEST_WIDTH;
    info->height = FB_TEST_HEIGHT;
    info->pitch = FB_TEST_WIDTH;
    info->columns = FB_TEST_WIDTH / 8u;
    info->rows = FB_TEST_HEIGHT / 16u;
    info->bpp = FB_BPP_RGB565;
    info->format = FB_FORMAT_RGB565;
    info->font_width = 8u;
    info->font_height = 16u;
}

/* A write of /dev/fb0 as the port sees it: the header, then the pixels. */
static k_size_t build_write(k_u8_t *buffer, k_u16_t x, k_u16_t y, k_u16_t w, k_u16_t h)
{
    struct fb_rect header;
    k_u16_t *pixels = (k_u16_t *)(void *)(buffer + sizeof(header));
    k_size_t count = (k_size_t)w * (k_size_t)h;
    k_size_t index;

    header.x = x;
    header.y = y;
    header.w = w;
    header.h = h;
    memcpy(buffer, &header, sizeof(header));

    for (index = 0; index < count; index++) {
        pixels[index] = (k_u16_t)(0x1000u + index);
    }

    return sizeof(header) + count * FB_PIXEL_BYTES;
}

static void test_fb_rect(void)
{
    struct fb_info info;
    struct fb_rect rect;
    const k_u16_t *pixels = NULL;
    k_u8_t buffer[64];
    k_size_t size;

    ktest_begin("fb: a rectangle of the picture is checked and its pixels found");

    info_default(&info);

    size = build_write(buffer, 2, 3, 4, 3);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, &pixels), 0);
    KT_CHECK_INT_EQ(rect.x, 2);
    KT_CHECK_INT_EQ(rect.y, 3);
    KT_CHECK_INT_EQ(rect.w, 4);
    KT_CHECK_INT_EQ(rect.h, 3);
    KT_CHECK(pixels != NULL);
    if (pixels != NULL) {
        KT_CHECK_INT_EQ(pixels[0], 0x1000);
        KT_CHECK_INT_EQ(pixels[11], 0x1000 + 11);
    }

    /* The last pixel of the picture is inside it, the one after is not. */
    size = build_write(buffer, FB_TEST_WIDTH - 2u, FB_TEST_HEIGHT - 2u, 2u, 2u);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, &pixels), 0);

    /* A rectangle that leaves the picture is refused, and it is not clipped:
     * the ADR promises an error and a program that sees it. */
    size = build_write(buffer, FB_TEST_WIDTH - 1u, 0u, 2u, 2u);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, &pixels), KERR_INVAL);

    size = build_write(buffer, 0u, FB_TEST_HEIGHT - 1u, 2u, 2u);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, &pixels), KERR_INVAL);

    size = build_write(buffer, FB_TEST_WIDTH, 0u, 1u, 1u);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, &pixels), KERR_INVAL);

    size = build_write(buffer, 0u, FB_TEST_HEIGHT, 1u, 1u);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, &pixels), KERR_INVAL);

    /* A rectangle of no pixels is a mistake, not a no-op. */
    size = build_write(buffer, 0u, 0u, 0u, 2u);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, &pixels), KERR_INVAL);

    size = build_write(buffer, 0u, 0u, 2u, 0u);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, &pixels), KERR_INVAL);

    /* The payload is exactly the pixels of the rectangle: a short write and
     * a long one are two different mistakes with one answer. */
    size = build_write(buffer, 0u, 0u, 2u, 2u);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size - 1u, &rect, &pixels), KERR_INVAL);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size + 1u, &rect, &pixels), KERR_INVAL);

    /* A buffer shorter than the header, and a caller without a place. */
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, sizeof(struct fb_rect) - 1u, &rect, &pixels),
                    KERR_INVAL);
    KT_CHECK_INT_EQ(fb_rect_parse(NULL, buffer, size, &rect, &pixels), KERR_INVAL);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, NULL, size, &rect, &pixels), KERR_INVAL);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, NULL, &pixels), KERR_INVAL);
    KT_CHECK_INT_EQ(fb_rect_parse(&info, buffer, size, &rect, NULL), KERR_INVAL);
}

static void test_input_names(void)
{
    ktest_begin("input: a key of the machine has one name for every program");

    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_A), "key.a");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_Q), "key.q");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_ENTER), "key.enter");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_TAB), "key.tab");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_F1), "key.f1");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_F12), "key.f12");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_NONE), "key.none");

    /* A number that is not a key of the contract reads as the empty name and
     * not as a key of somebody else. */
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_COUNT), "key.none");
    KT_CHECK_STR_EQ(k_input_key_name(0xFFFFu), "key.none");

    /* The event is eight bytes: the device and a program agree about the
     * layout of one read. */
    KT_CHECK_INT_EQ((int)sizeof(struct input_event), 8);
    KT_CHECK_INT_EQ((int)offsetof(struct input_event, code), 4);
    KT_CHECK_INT_EQ((int)offsetof(struct input_event, value), 6);
}

void test_ui(void)
{
    test_fb_rect();
    test_input_names();
}
