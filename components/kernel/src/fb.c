/*
 * Micro OS — the rectangle of a framebuffer write (ADR 0028).
 *
 * The port that owns a picture reads the header a program wrote and checks it
 * here, in portable code: the shape of a write is the same on every machine
 * that offers `/dev/fb0`, and a test of this file is the test of the contract
 * the machines share.
 */
#include "kernel/fb.h"

#include <string.h>

#include "kernel/errno.h"

int fb_rect_parse(const struct fb_info *info, const void *buffer, k_size_t size,
                  struct fb_rect *out, const k_u16_t **pixels)
{
    struct fb_rect rect;
    k_u64_t count;
    k_u64_t expected;

    if (info == NULL || buffer == NULL || out == NULL || pixels == NULL) {
        return KERR_INVAL;
    }

    if (size < sizeof(rect)) {
        return KERR_INVAL;
    }

    memcpy(&rect, buffer, sizeof(rect));

    /* A rectangle of no pixels is not a drawing: it is a mistake of the
     * caller, and refusing it keeps a write of eight bytes from being a
     * silent no-op. */
    if (rect.w == 0u || rect.h == 0u) {
        return KERR_INVAL;
    }

    /* The right and the bottom edge belong to the picture: a rectangle that
     * ends exactly at the last pixel is whole, one pixel more is outside. */
    if ((k_u32_t)rect.x + (k_u32_t)rect.w > (k_u32_t)info->width ||
        (k_u32_t)rect.y + (k_u32_t)rect.h > (k_u32_t)info->height) {
        return KERR_INVAL;
    }

    count = (k_u64_t)rect.w * (k_u64_t)rect.h;
    expected = sizeof(rect) + count * (k_u64_t)FB_PIXEL_BYTES;

    /* The payload is exactly the pixels of the rectangle: a shorter write is
     * a broken frame and a longer one is a caller that believes another
     * shape. */
    if ((k_u64_t)size != expected) {
        return KERR_INVAL;
    }

    *out = rect;
    *pixels = (const k_u16_t *)(const void *)((const k_u8_t *)buffer + sizeof(rect));

    return 0;
}
