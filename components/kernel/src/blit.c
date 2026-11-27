/*
 * Micro OS — the blit of a picture (kernel/blit.h).
 *
 * The software path: a fill is a word store, a copy is a row at a time with
 * the overlap of one surface handled as memmove handles it, and a flush of a
 * memory the display reads is nothing. A device that registered operations
 * through `kernel/fb_ops.h` answers the operations it has; the software path
 * answers the rest.
 */
#include "kernel/blit.h"

#include <string.h>

#include "kernel/errno.h"
#include "kernel/fb.h"
#include "kernel/log.h"
#include "kernel/port/exec.h"

/* A card which cannot flip is not asked once per frame. The identity and
 * registration generation of the backend are part of the latch: a newly
 * registered driver gets its one attempt, while repeated frames do not. */
static const struct k_fb_ops *s_present_ops;
static void *s_present_context;
static k_u32_t s_present_generation;
static bool s_present_failed;

static bool clip_rect(const k_blit_surface *surface, const k_blit_rect *rect, k_blit_rect *out)
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

static int software_fill(k_blit_surface *surface, const k_blit_rect *rect, k_u32_t pixel)
{
    k_blit_rect part;
    k_i32_t row;

    if (!clip_rect(surface, rect, &part)) {
        return 0;
    }

    if (surface->format == FB_FORMAT_XRGB8888) {
        k_u32_t *base = (k_u32_t *)(void *)surface->base;
        for (row = 0; row < part.h; row++) {
            k_u32_t *line =
                base + (k_size_t)(part.y + row) * (k_size_t)surface->pitch + (k_size_t)part.x;
            k_i32_t column = 0;
            k_i32_t count = part.w;
            k_u64_t word = ((k_u64_t)pixel << 32) | (k_u64_t)pixel;

            while (count > 0 && (((uintptr_t)(line + column)) & 7u) != 0u) {
                line[column] = pixel;
                column++;
                count--;
            }
            while (count >= 2) {
                __builtin_memcpy(line + column, &word, sizeof(word));
                column += 2;
                count -= 2;
            }
            while (count > 0) {
                line[column] = pixel;
                column++;
                count--;
            }
        }
        return 0;
    }

    for (row = 0; row < part.h; row++) {
        k_u16_t *line =
            surface->base + (k_size_t)(part.y + row) * (k_size_t)surface->pitch + (k_size_t)part.x;
        k_i32_t column = 0;
        k_i32_t count = part.w;
        k_u16_t px = (k_u16_t)pixel;
        k_u64_t word = (k_u64_t)px;

        word |= word << 16;
        word |= word << 32;

        while (count > 0 && (((uintptr_t)(line + column)) & 7u) != 0u) {
            line[column] = px;
            column++;
            count--;
        }
        while (count >= 4) {
            __builtin_memcpy(line + column, &word, sizeof(word));
            column += 4;
            count -= 4;
        }
        while (count > 0) {
            line[column] = px;
            column++;
            count--;
        }
    }

    return 0;
}

int k_blit_fill(k_blit_surface *surface, const k_blit_rect *rect, k_u32_t color)
{
    void *context;
    const struct k_fb_ops *ops = k_fb_ops_device(&context);

    if (ops != NULL && ops->fill != NULL) {
        return ops->fill(context, rect, color);
    }

    return software_fill(surface, rect, color);
}

static int software_copy(k_blit_surface *dst, const k_blit_rect *dst_rect,
                         const k_blit_surface *src, const k_blit_rect *src_rect)
{
    k_blit_rect dpart;
    k_i32_t row;
    k_i32_t sx;
    k_i32_t sy;
    k_i32_t dx;
    k_i32_t dy;
    k_i32_t w;
    k_i32_t h;
    bool reverse;

    if (dst == NULL || src == NULL || dst_rect == NULL || src_rect == NULL) {
        return KERR_INVAL;
    }

    if (dst_rect->w <= 0 || dst_rect->h <= 0 || src_rect->w <= 0 || src_rect->h <= 0) {
        return 0;
    }

    w = dst_rect->w < src_rect->w ? dst_rect->w : src_rect->w;
    h = dst_rect->h < src_rect->h ? dst_rect->h : src_rect->h;

    /* Clip destination. */
    {
        k_blit_rect want;

        want.x = dst_rect->x;
        want.y = dst_rect->y;
        want.w = w;
        want.h = h;

        if (!clip_rect(dst, &want, &dpart)) {
            return 0;
        }
    }

    /* Adjust source by the same clip delta. */
    sx = src_rect->x + (dpart.x - dst_rect->x);
    sy = src_rect->y + (dpart.y - dst_rect->y);
    dx = dpart.x;
    dy = dpart.y;
    w = dpart.w;
    h = dpart.h;

    /* Clip source. */
    if (sx < 0) {
        dx -= sx;
        w += sx;
        sx = 0;
    }
    if (sy < 0) {
        dy -= sy;
        h += sy;
        sy = 0;
    }
    if (sx + w > (k_i32_t)src->width) {
        w = (k_i32_t)src->width - sx;
    }
    if (sy + h > (k_i32_t)src->height) {
        h = (k_i32_t)src->height - sy;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }

    /* Clip destination again after source clip. */
    if (dx < 0) {
        sx -= dx;
        w += dx;
        dx = 0;
    }
    if (dy < 0) {
        sy -= dy;
        h += dy;
        dy = 0;
    }
    if (dx + w > (k_i32_t)dst->width) {
        w = (k_i32_t)dst->width - dx;
    }
    if (dy + h > (k_i32_t)dst->height) {
        h = (k_i32_t)dst->height - dy;
    }
    if (w <= 0 || h <= 0) {
        return 0;
    }

    {
        k_u8_t pixel_bytes =
            fb_pixel_bytes(dst->format == FB_FORMAT_XRGB8888 ? dst->format : src->format);

        reverse = (dst->base == src->base) && (dy > sy) && (dy < sy + h);

        if (reverse) {
            for (row = h - 1; row >= 0; row--) {
                k_u8_t *dline = (k_u8_t *)(void *)dst->base +
                                ((k_size_t)(dy + row) * (k_size_t)dst->pitch + (k_size_t)dx) *
                                    (k_size_t)pixel_bytes;
                const k_u8_t *sline = (const k_u8_t *)(const void *)src->base +
                                      ((k_size_t)(sy + row) * (k_size_t)src->pitch + (k_size_t)sx) *
                                          (k_size_t)pixel_bytes;

                memmove(dline, sline, (k_size_t)w * (k_size_t)pixel_bytes);
            }
        } else {
            for (row = 0; row < h; row++) {
                k_u8_t *dline = (k_u8_t *)(void *)dst->base +
                                ((k_size_t)(dy + row) * (k_size_t)dst->pitch + (k_size_t)dx) *
                                    (k_size_t)pixel_bytes;
                const k_u8_t *sline = (const k_u8_t *)(const void *)src->base +
                                      ((k_size_t)(sy + row) * (k_size_t)src->pitch + (k_size_t)sx) *
                                          (k_size_t)pixel_bytes;

                /* memmove handles the same-row overlap case. */
                memmove(dline, sline, (k_size_t)w * (k_size_t)pixel_bytes);
            }
        }
    }

    return 0;
}

int k_blit_copy(k_blit_surface *dst, const k_blit_rect *dst_rect, const k_blit_surface *src,
                const k_blit_rect *src_rect)
{
    return software_copy(dst, dst_rect, src, src_rect);
}

static bool present_surfaces_valid(const k_blit_surface *dst, const k_blit_surface *src)
{
    k_u64_t bytes;
    k_u8_t pixel_bytes;
    k_u8_t dst_fmt;
    k_u8_t src_fmt;

    if (dst == NULL || src == NULL || dst->base == NULL || src->base == NULL || dst->width == 0u ||
        dst->height == 0u || src->width != dst->width || src->height != dst->height ||
        dst->width > 0x7FFFFFFFu || dst->height > 0x7FFFFFFFu || dst->pitch < dst->width ||
        src->pitch < src->width) {
        return false;
    }

    dst_fmt = dst->format == FB_FORMAT_XRGB8888 ? FB_FORMAT_XRGB8888 : FB_FORMAT_RGB565;
    src_fmt = src->format == FB_FORMAT_XRGB8888 ? FB_FORMAT_XRGB8888 : FB_FORMAT_RGB565;
    if (dst_fmt != src_fmt) {
        return false;
    }

    pixel_bytes = fb_pixel_bytes(src_fmt);
    bytes = (k_u64_t)src->pitch * (k_u64_t)src->height * (k_u64_t)pixel_bytes;
    if (bytes > (k_u64_t)((k_size_t)-1)) {
        return false;
    }

    return k_port_exec_contains(src->base, (k_size_t)bytes);
}

int k_blit_present(k_blit_surface *dst, const k_blit_surface *src)
{
    void *context;
    const struct k_fb_ops *ops = k_fb_ops_device(&context);
    k_u32_t generation = k_fb_ops_generation();
    k_blit_rect whole;
    int rc;

    if (!present_surfaces_valid(dst, src)) {
        return KERR_INVAL;
    }

    if (ops != s_present_ops || context != s_present_context ||
        generation != s_present_generation) {
        s_present_ops = ops;
        s_present_context = context;
        s_present_generation = generation;
        s_present_failed = false;
    }

    if (ops != NULL && ops->present != NULL && !s_present_failed) {
        rc = ops->present(context, dst, src);
        if (rc == 0) {
            return 0;
        }

        /* A refusal is a capability answer, not a frame error. The copy below
         * is the same software path a machine without a GPU has always used. */
        s_present_failed = true;
        kwarn("blit: present refused (rc=%d), using software copy fallback", rc);
    } else if (ops == NULL || ops->present == NULL) {
        s_present_failed = true;
    }

    whole.x = 0;
    whole.y = 0;
    whole.w = (k_i32_t)src->width;
    whole.h = (k_i32_t)src->height;

    k_port_exec_user_access_begin();
    rc = software_copy(dst, &whole, src, &whole);
    k_port_exec_user_access_end();

    return rc == 0 ? 1 : rc;
}

int k_blit_present_release(void)
{
    void *context;
    const struct k_fb_ops *ops = k_fb_ops_device(&context);

    if (ops == NULL || ops->present_release == NULL) {
        return 0;
    }

    return ops->present_release(context);
}

int k_blit_flush(k_blit_surface *surface, const k_blit_rect *rects, k_u32_t count)
{
    void *context;
    const struct k_fb_ops *ops = k_fb_ops_device(&context);
    k_u32_t index;

    if (surface == NULL || surface->base == NULL) {
        return KERR_INVAL;
    }

    if (ops != NULL && ops->flush != NULL) {
        if (rects == NULL || count == 0u) {
            k_blit_rect whole;

            whole.x = 0;
            whole.y = 0;
            whole.w = (k_i32_t)surface->width;
            whole.h = (k_i32_t)surface->height;

            return ops->flush(context, &whole);
        }

        for (index = 0; index < count; index++) {
            int rc = ops->flush(context, &rects[index]);

            if (rc != 0) {
                return rc;
            }
        }

        return 0;
    }

    if (rects == NULL || count == 0u) {
        return 0;
    }

    for (index = 0; index < count; index++) {
        if (rects[index].w < 0 || rects[index].h < 0) {
            return KERR_INVAL;
        }
    }

    return 0;
}
