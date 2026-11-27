/*
 * Micro OS — what a framebuffer may do faster than the program (1.7).
 *
 * `/dev/fb0` is a rectangle of pixels and a memory the port copies into
 * (ADR 0028): a program composes a rectangle, writes it, and the port does
 * the `memcpy`. A card that can fill a rectangle or copy one in its own
 * engine does that work itself, and the machine of this system - the VGA of
 * QEMU - has no such engine. So this file is a contract and not a driver:
 * the shape of the three operations a device may offer, the slot where it
 * offers them, and the software implementation that answers when the slot is
 * empty. The software path is the one of 1.6 and it stays the only one that
 * is always there. A complete RAM surface may also be presented by flipping
 * the display to it; a device which cannot do that keeps the software copy.
 *
 * The numbers are the pixels of the picture the device was given: the
 * software path of this release writes the low sixteen bits of `pixel`, which
 * is the RGB565 of the screen of the port (ADR 0023), and a device of another
 * depth reads the whole word of a colour of its own.
 *
 * Rectangle that leaves the picture: the software path clips it (a part of it
 * is drawn) and a device answers for its own rectangle as it does for its own
 * pixels. The device of `/dev/fb0` is given a rectangle the ABI checked, so
 * the clip is the rule of a caller inside the kernel and not a second check
 * of the syscall.
 */
#ifndef MICRO_OS_KERNEL_FB_OPS_H
#define MICRO_OS_KERNEL_FB_OPS_H

#include "kernel/types.h"

/*
 * A rectangle of pixels of a picture: the destination of an operation. The
 * point is signed, because a caller inside the kernel composes a rectangle
 * that may begin outside the picture and the software path cuts it; the
 * device of `/dev/fb0` is given a rectangle the ABI already checked.
 */
struct k_fb_rect {
    k_i32_t x;
    k_i32_t y;
    k_i32_t w;
    k_i32_t h;
};

/*
 * The software picture: the memory the pixels are in, the shape of it and the
 * step of a line. A device of the system is one of these plus the operations
 * it can do itself; a picture with no operations at all is the whole of what
 * 1.6 has.
 */
struct k_fb_surface {
    k_u16_t *base;  /* the first pixel of the first line */
    k_u32_t width;  /* the pixels of a line of the picture */
    k_u32_t height; /* the lines of the picture */
    k_u32_t pitch;  /* the pixels from one line to the next, padding included */
};

/*
 * The operations of a device. All four may be NULL: an operation a device
 * does not have is the software one, and a device with no operations is the
 * machine of 1.6.
 *
 * `fill` paints the rectangle with the colour of the picture; `blit` copies
 * the rectangle `src_x, src_y, w, h` of a block of pixels of `src_pitch`
 * pixels a line into `dst`; `flush` makes a rectangle of the picture visible
 * to the display that is not the memory (the software picture of this system
 * needs nothing, and its flush is the empty answer).
 *
 * `present` shows a complete surface instead of copying it into `dst`.
 * The surfaces have the same shape and pixel format. KERR_NOTSUP is a
 * capability answer and `k_blit_present` falls back to the software copy;
 * another negative error is propagated after the driver has attempted its
 * rollback, because copying into an unseen destination would hide the fault.
 * `present_release` restores the original surface before the owner gives the
 * device back. A refusal is remembered per source, so a RAM surface does not
 * disable a later device surface.
 */
struct k_fb_ops {
    int (*fill)(void *context, const struct k_fb_rect *rect, k_u32_t pixel);
    int (*blit)(void *context, const struct k_fb_rect *dst, const void *src, k_u32_t src_pitch,
                k_u32_t src_x, k_u32_t src_y);
    int (*flush)(void *context, const struct k_fb_rect *rect);
    int (*present)(void *context, const struct k_fb_surface *dst, const struct k_fb_surface *src);
    int (*present_release)(void *context);
};

/*
 * The device of the picture offers its operations, or takes them back with
 * NULL when it goes away. There is one picture of this size in the system and
 * one slot for it: the port of a machine calls this at the boot, and a device
 * that is not there leaves the slot empty.
 */
int k_fb_ops_register(const struct k_fb_ops *ops, void *context);

/* The operations of the picture and their context; NULL when the software
 * path is the whole of it. */
const struct k_fb_ops *k_fb_ops_device(void **context);

/* Changes whenever the slot is registered or cleared, even when the same
 * operations structure is registered again. */
k_u32_t k_fb_ops_generation(void);

/*
 * The three operations as the system asks for them: the device answers the
 * ones it has and `surface` is the software picture for the rest. The answer
 * is 0, or a negative number of the ABI (`KERR_INVAL` for a picture that is
 * not one, `KERR_NOTSUP` for a flush the software picture cannot do).
 */
int k_fb_fill(const struct k_fb_surface *surface, const struct k_fb_rect *rect, k_u32_t pixel);
int k_fb_blit(const struct k_fb_surface *surface, const struct k_fb_rect *dst, const void *src,
              k_u32_t src_pitch, k_u32_t src_x, k_u32_t src_y);
int k_fb_flush(const struct k_fb_surface *surface, const struct k_fb_rect *rect);

#endif /* MICRO_OS_KERNEL_FB_OPS_H */
