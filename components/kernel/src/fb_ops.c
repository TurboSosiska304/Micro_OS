/*
 * Micro OS — the operations of the picture (kernel/fb_ops.h).
 *
 * One slot for the device of `/dev/fb0` and the software implementations of
 * the rectangle operations. The slot is written once at the boot by the port and
 * read by every caller afterwards, so it needs no lock of its own: a device
 * that arrives after the first frame is a device of the next boot.
 *
 * The software path is the copy of 1.6 with the bounds of the picture around
 * it: a rectangle that leaves the picture is cut to it, which is the rule of
 * a drawing that composes - the device of ADR 0028 refuses a rectangle that
 * leaves the screen at the door of the syscall, and a caller inside the
 * kernel has no such door.
 */
#include "kernel/fb_ops.h"

#include <string.h>

#include "kernel/errno.h"

static const struct k_fb_ops *g_ops;
static void *g_context;
static k_u32_t g_generation;

int k_fb_ops_register(const struct k_fb_ops *ops, void *context)
{
    g_ops = ops;
    g_context = ops != NULL ? context : NULL;
    g_generation++;

    return 0;
}

const struct k_fb_ops *k_fb_ops_device(void **context)
{
    if (context != NULL) {
        *context = g_context;
    }

    return g_ops;
}

/* The registration counter lets a consumer remember a refusal without
 * treating a newly registered driver as the same backend. */
k_u32_t k_fb_ops_generation(void)
{
    return g_generation;
}

/* The part of a rectangle that is inside the picture, false when none is. */
static bool clip(const struct k_fb_surface *surface, const struct k_fb_rect *rect,
                 struct k_fb_rect *out)
{
    k_i64_t left;
    k_i64_t top;
    k_i64_t right;
    k_i64_t bottom;

    if (surface == NULL || surface->base == NULL || rect == NULL || out == NULL || rect->w <= 0 ||
        rect->h <= 0) {
        return false;
    }

    left = rect->x < 0 ? 0 : rect->x;
    top = rect->y < 0 ? 0 : rect->y;
    right = (k_i64_t)rect->x + rect->w;
    bottom = (k_i64_t)rect->y + rect->h;

    if (right > (k_i64_t)surface->width) {
        right = surface->width;
    }
    if (bottom > (k_i64_t)surface->height) {
        bottom = surface->height;
    }
    if (left >= right || top >= bottom) {
        return false;
    }

    out->x = (k_i32_t)left;
    out->y = (k_i32_t)top;
    out->w = (k_i32_t)(right - left);
    out->h = (k_i32_t)(bottom - top);

    return true;
}

/* The software fill: a word of four pixels at a time, so a rectangle of
 * one colour does not cost a store a pixel. */
static int software_fill(const struct k_fb_surface *surface, const struct k_fb_rect *rect,
                         k_u16_t pixel)
{
    struct k_fb_rect part;
    k_i32_t row;

    if (!clip(surface, rect, &part)) {
        return 0;
    }

    for (row = 0; row < part.h; row++) {
        k_u16_t *line =
            surface->base + (k_size_t)(part.y + row) * (k_size_t)surface->pitch + (k_size_t)part.x;
        k_i32_t column = 0;
        k_i32_t count = part.w;
        k_u64_t word = (k_u64_t)pixel;

        word |= word << 16;
        word |= word << 32;

        while (count > 0 && (((uintptr_t)(line + column)) & 7u) != 0u) {
            line[column] = pixel;
            column++;
            count--;
        }

        while (count >= 4) {
            __builtin_memcpy(line + column, &word, sizeof(word));
            column += 4;
            count -= 4;
        }

        while (count > 0) {
            line[column] = pixel;
            column++;
            count--;
        }
    }

    return 0;
}

int k_fb_fill(const struct k_fb_surface *surface, const struct k_fb_rect *rect, k_u32_t pixel)
{
    void *context;

    if (g_ops != NULL && g_ops->fill != NULL) {
        (void)k_fb_ops_device(&context);
        return g_ops->fill(context, rect, pixel);
    }

    return software_fill(surface, rect, (k_u16_t)pixel);
}

/* The software blit: the rectangle of the source, a line at a time. */
static int software_blit(const struct k_fb_surface *surface, const struct k_fb_rect *dst,
                         const void *src, k_u32_t src_pitch, k_u32_t src_x, k_u32_t src_y)
{
    const k_u16_t *pixels = (const k_u16_t *)src;
    struct k_fb_rect part;
    k_i32_t row;

    if (pixels == NULL || src_pitch == 0u) {
        return KERR_INVAL;
    }

    if (!clip(surface, dst, &part)) {
        return 0;
    }

    /* The same clip on the source: a rectangle that starts left of the source
     * reads the cells it can and not the ones before them. */
    if (src_x >= src_pitch) {
        return KERR_INVAL;
    }

    for (row = 0; row < part.h; row++) {
        const k_u16_t *source = pixels + (k_size_t)(src_y + row) * (k_size_t)src_pitch +
                                (k_size_t)src_x + (k_size_t)(part.x - dst->x);
        k_u16_t *line =
            surface->base + (k_size_t)(part.y + row) * (k_size_t)surface->pitch + (k_size_t)part.x;

        memcpy(line, source, (k_size_t)part.w * sizeof(k_u16_t));
    }

    return 0;
}

int k_fb_blit(const struct k_fb_surface *surface, const struct k_fb_rect *dst, const void *src,
              k_u32_t src_pitch, k_u32_t src_x, k_u32_t src_y)
{
    void *context;

    if (g_ops != NULL && g_ops->blit != NULL) {
        (void)k_fb_ops_device(&context);
        return g_ops->blit(context, dst, src, src_pitch, src_x, src_y);
    }

    return software_blit(surface, dst, src, src_pitch, src_x, src_y);
}

int k_fb_flush(const struct k_fb_surface *surface, const struct k_fb_rect *rect)
{
    void *context;

    if (g_ops != NULL && g_ops->flush != NULL) {
        (void)k_fb_ops_device(&context);
        return g_ops->flush(context, rect);
    }

    /* The picture of this system is the memory the display reads: there is
     * nothing to make visible. */
    if (surface == NULL || surface->base == NULL || rect == NULL) {
        return KERR_INVAL;
    }

    return 0;
}
