/*
 * Micro OS — the blit of a picture (the phase A of the GPU plan).
 *
 * A picture is drawn in a memory the CPU can write, and the part of it that
 * changed is given to the device of the display. The operations below are
 * the whole of that: fill a rectangle, copy a rectangle from one picture to
 * another, make rectangles visible, and present a complete RAM surface by
 * flipping the display to it. A device that can do one of them itself answers
 * through the same slot as `kernel/fb_ops.h`; a device that can do none of them
 * is the software picture of 1.6 and the software path is what it gets.
 *
 * The interface is the seam: a compositor composes into a surface of ordinary
 * RAM and calls flush with the damage of the turn, and the same surface can
 * later be handed to a device backend without the compositor knowing which
 * one it is (ADR 0004).
 */
#ifndef MICRO_OS_KERNEL_BLIT_H
#define MICRO_OS_KERNEL_BLIT_H

#include "kernel/fb_ops.h"

/* The picture and the rectangle are the ones of the device contract: the
 * same memory, the same pitch in pixels, the same signed rectangle that a
 * composition clips. */
typedef struct k_fb_surface k_blit_surface;
typedef struct k_fb_rect k_blit_rect;

/* Paint `rect` of `surface` with `color`. 0 on success. */
int k_blit_fill(k_blit_surface *surface, const k_blit_rect *rect, k_u32_t color);

/*
 * Copy `src_rect` of `src` into `dst_rect` of `dst`. The two rectangles have
 * the same size; a source or a destination that leaves its picture is cut to
 * the part that is inside both. Overlap inside the same surface is handled as
 * memmove handles it: the pixels read are the ones the source had before the
 * copy.
 */
int k_blit_copy(k_blit_surface *dst, const k_blit_rect *dst_rect, const k_blit_surface *src,
                const k_blit_rect *src_rect);

/*
 * Make `count` rectangles of `surface` visible. NULL `rects` or `count == 0`
 * means the whole surface. A software picture has nothing to present and
 * answers 0; a device picture flushes the union it was given.
 */
int k_blit_flush(k_blit_surface *surface, const k_blit_rect *rects, k_u32_t count);

/*
 * Show the complete RAM surface `src` instead of copying it into `dst`. The
 * surfaces have the same dimensions and pixel format. A device backend gets
 * the first attempt; if it is absent or refuses, the pixels are copied into
 * `dst` exactly as `k_blit_copy` would copy them. A refusal is remembered for
 * that backend, so an unsupported card is not probed once per frame.
 *
 * The answer is 0 when the device showed `src`, 1 when the software copy was
 * used, or a negative error when even that copy was impossible. A fallback is
 * a successful present, not an error. `k_blit_present_release` asks the backend
 * to restore the original surface before its caller releases the source pages.
 */
int k_blit_present(k_blit_surface *dst, const k_blit_surface *src);
int k_blit_present_release(void);

#endif /* MICRO_OS_KERNEL_BLIT_H */
