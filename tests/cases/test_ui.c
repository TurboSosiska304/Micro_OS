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
#include "kernel/input_set1.h"

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

    /* The keys of a moving cursor arrived in 1.4.0 and their numbers are
     * after the frozen block: a key added to the list never moves another. */
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_UP), "key.up");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_DOWN), "key.down");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_LEFT), "key.left");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_RIGHT), "key.right");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_HOME), "key.home");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_END), "key.end");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_PAGEUP), "key.pageup");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_PAGEDOWN), "key.pagedown");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_INSERT), "key.insert");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_DELETE), "key.delete");

    /* The pointer of the machine arrived in 1.7.0, after those numbers. */
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_POINTER_X), "key.pointer.x");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_POINTER_Y), "key.pointer.y");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_BUTTON_LEFT), "key.button.left");
    KT_CHECK_STR_EQ(k_input_key_name(K_INPUT_KEY_POINTER_WHEEL), "key.pointer.wheel");

    KT_CHECK_INT_EQ(K_INPUT_KEY_A, 1);
    KT_CHECK_INT_EQ(K_INPUT_KEY_Z, 26);
    KT_CHECK_INT_EQ(K_INPUT_KEY_0, 27);
    KT_CHECK_INT_EQ(K_INPUT_KEY_ENTER, 37);
    KT_CHECK_INT_EQ(K_INPUT_KEY_F12, 68);
    KT_CHECK_INT_EQ(K_INPUT_KEY_UP, 69);
    KT_CHECK_INT_EQ(K_INPUT_KEY_DELETE, 78);
    KT_CHECK_INT_EQ(K_INPUT_KEY_POINTER_X, 79);
    KT_CHECK_INT_EQ(K_INPUT_KEY_POINTER_Y, 80);
    KT_CHECK_INT_EQ(K_INPUT_KEY_BUTTON_LEFT, 81);
    KT_CHECK_INT_EQ(K_INPUT_KEY_POINTER_WHEEL, 82);
    KT_CHECK_INT_EQ(K_INPUT_KEY_COUNT, 83);

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

static void test_input_set1(void)
{
    ktest_begin("input: the scancodes of the set 1 are the keys of the machine");

    /* The letters and the digits of the first block. */
    KT_CHECK_INT_EQ(k_input_set1_key(0x1E, false), K_INPUT_KEY_A);
    KT_CHECK_INT_EQ(k_input_set1_key(0x2C, false), K_INPUT_KEY_Z);
    KT_CHECK_INT_EQ(k_input_set1_key(0x02, false), K_INPUT_KEY_1);
    KT_CHECK_INT_EQ(k_input_set1_key(0x0B, false), K_INPUT_KEY_0);
    KT_CHECK_INT_EQ(k_input_set1_key(0x1C, false), K_INPUT_KEY_ENTER);
    KT_CHECK_INT_EQ(k_input_set1_key(0x0F, false), K_INPUT_KEY_TAB);
    KT_CHECK_INT_EQ(k_input_set1_key(0x1D, false), K_INPUT_KEY_CTRL);
    KT_CHECK_INT_EQ(k_input_set1_key(0x2A, false), K_INPUT_KEY_SHIFT);
    KT_CHECK_INT_EQ(k_input_set1_key(0x3B, false), K_INPUT_KEY_F1);
    KT_CHECK_INT_EQ(k_input_set1_key(0x58, false), K_INPUT_KEY_F12);

    /* The arrows and the navigation of the second form: the same address
     * without the E0 prefix is the keypad, which this console does not map
     * to a key of the contract - and that is the whole point of the flag. */
    KT_CHECK_INT_EQ(k_input_set1_key(0x48, true), K_INPUT_KEY_UP);
    KT_CHECK_INT_EQ(k_input_set1_key(0x50, true), K_INPUT_KEY_DOWN);
    KT_CHECK_INT_EQ(k_input_set1_key(0x4B, true), K_INPUT_KEY_LEFT);
    KT_CHECK_INT_EQ(k_input_set1_key(0x4D, true), K_INPUT_KEY_RIGHT);
    KT_CHECK_INT_EQ(k_input_set1_key(0x47, true), K_INPUT_KEY_HOME);
    KT_CHECK_INT_EQ(k_input_set1_key(0x4F, true), K_INPUT_KEY_END);
    KT_CHECK_INT_EQ(k_input_set1_key(0x49, true), K_INPUT_KEY_PAGEUP);
    KT_CHECK_INT_EQ(k_input_set1_key(0x51, true), K_INPUT_KEY_PAGEDOWN);
    KT_CHECK_INT_EQ(k_input_set1_key(0x52, true), K_INPUT_KEY_INSERT);
    KT_CHECK_INT_EQ(k_input_set1_key(0x53, true), K_INPUT_KEY_DELETE);
    KT_CHECK_INT_EQ(k_input_set1_key(0x48, false), K_INPUT_KEY_NONE);
    KT_CHECK_INT_EQ(k_input_set1_key(0x47, false), K_INPUT_KEY_NONE);
    KT_CHECK_INT_EQ(k_input_set1_key(0x1E, true), K_INPUT_KEY_NONE);

    /* The pad and the right-hand modifiers of the second form. */
    KT_CHECK_INT_EQ(k_input_set1_key(0x1C, true), K_INPUT_KEY_ENTER);
    KT_CHECK_INT_EQ(k_input_set1_key(0x35, true), K_INPUT_KEY_SLASH);
    KT_CHECK_INT_EQ(k_input_set1_key(0x1D, true), K_INPUT_KEY_CTRL);
    KT_CHECK_INT_EQ(k_input_set1_key(0x38, true), K_INPUT_KEY_ALT);

    /* An address that is no key at all answers the empty key, in both
     * blocks. */
    KT_CHECK_INT_EQ(k_input_set1_key(0x00, false), K_INPUT_KEY_NONE);
    KT_CHECK_INT_EQ(k_input_set1_key(0x7F, false), K_INPUT_KEY_NONE);
    KT_CHECK_INT_EQ(k_input_set1_key(0x00, true), K_INPUT_KEY_NONE);
    KT_CHECK_INT_EQ(k_input_set1_key(0x7F, true), K_INPUT_KEY_NONE);
}

void test_ui(void)
{
    test_fb_rect();
    test_input_names();
    test_input_set1();
}
