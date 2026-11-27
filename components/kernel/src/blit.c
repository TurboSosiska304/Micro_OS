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

static void fill_row(k_u16_t *line, k_i32_t count, k_u16_t pixel)
{
    k_i32_t column = 0;
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

static int software_fill(k_blit_surface *surface, const k_blit_rect *rect, k_u16_t pixel)
{
    k_blit_rect part;
    k_i32_t row;

    if (!clip_rect(surface, rect, &part)) {
        return 0;
    }

    for (row = 0; row < part.h; row++) {
        k_u16_t *line =
            surface->base + (k_size_t)(part.y + row) * (k_size_t)surface->pitch + (k_size_t)part.x;

        fill_row(line, part.w, pixel);
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

    return software_fill(surface, rect, (k_u16_t)color);
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

    reverse = (dst->base == src->base) && (dy > sy) && (dy < sy + h);

    if (reverse) {
        for (row = h - 1; row >= 0; row--) {
            k_u16_t *dline = dst->base + (k_size_t)(dy + row) * (k_size_t)dst->pitch + (k_size_t)dx;
            const k_u16_t *sline =
                src->base + (k_size_t)(sy + row) * (k_size_t)src->pitch + (k_size_t)sx;

            memmove(dline, sline, (k_size_t)w * sizeof(k_u16_t));
        }
    } else {
        for (row = 0; row < h; row++) {
            k_u16_t *dline = dst->base + (k_size_t)(dy + row) * (k_size_t)dst->pitch + (k_size_t)dx;
            const k_u16_t *sline =
                src->base + (k_size_t)(sy + row) * (k_size_t)src->pitch + (k_size_t)sx;

            /* memmove handles the same-row overlap case. */
            memmove(dline, sline, (k_size_t)w * sizeof(k_u16_t));
        }
    }

    return 0;
}

int k_blit_copy(k_blit_surface *dst, const k_blit_rect *dst_rect, const k_blit_surface *src,
                const k_blit_rect *src_rect)
{
    return software_copy(dst, dst_rect, src, src_rect);
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
