/*
 * Micro OS — the framebuffer as a device (ADR 0028).
 *
 * A program that wants a screen asks the kernel for `/dev/fb0`: `read` gives
 * this description, `write` takes a rectangle of pixels. The shape is a blit
 * and not a mapping, because the ABI has no `mmap` and the arena of a program
 * is a fraction of a frame of the machine.
 *
 * The numbers of the control commands are part of the frozen ABI: they are
 * the ones of the release that introduced them and are never reused.
 */
#ifndef MICRO_OS_KERNEL_FB_H
#define MICRO_OS_KERNEL_FB_H

#include "kernel/types.h"

/* The bits of a pixel and the format of its number. The machine of 1.4 has
 * one format; a second one is a new number and not a change of this one. */
#define FB_BPP_RGB565 16u
#define FB_FORMAT_RGB565 1u

/* The bytes of one pixel of the format above. */
#define FB_PIXEL_BYTES 2u

/* The colours of the console palette a program may name in its own picture. */
#define FB_PALETTE_COLORS 16u

/*
 * What a `read` of `/dev/fb0` answers: the shape of the picture, the format
 * of a pixel and the grid of the console, so that a program draws text with
 * the same cell the kernel uses and chooses the same sixteen colours.
 *
 * `pitch` is the width of a line in pixels (a mode may pad a line); `columns`
 * and `rows` are the cells of the console's grid on this picture.
 */
struct fb_info {
    k_u16_t width;
    k_u16_t height;
    k_u16_t pitch;
    k_u16_t columns;
    k_u16_t rows;
    k_u8_t bpp;
    k_u8_t format;
    k_u16_t font_width;
    k_u16_t font_height;
    k_u16_t palette[FB_PALETTE_COLORS];
};

/* A rectangle of the picture: the header of a `write` of `/dev/fb0`. The
 * pixels follow it, `w * h` of them, one row after another, in the format
 * `fb_info` names. */
struct fb_rect {
    k_u16_t x;
    k_u16_t y;
    k_u16_t w;
    k_u16_t h;
};

/*
 * A complete RGB565 surface in the memory of a program. `base` is the first
 * pixel, `pitch` is the number of pixels from one line to the next, and the
 * range `pitch * height` must stay mapped until FBIO_GIVE (or the last close).
 * The shape must equal the `fb_info` of the device.
 */
struct fb_present_surface {
    k_u64_t base;
    k_u32_t width;
    k_u32_t height;
    k_u32_t pitch;
};

/*
 * The control commands of the picture. `FBIO_TAKE` stops the console drawing
 * into the framebuffer (the program owns the picture now); `FBIO_GIVE` hands
 * it back and the kernel repaints its grid. A program may have started
 * another program (the file manager starts the editor) and the screen is one
 * resource: a give while another descriptor of the device is open changes
 * nothing, and the last close gives the picture back whatever the programs
 * asked. A panic always takes the screen, whatever a program holds.
 *
 * `FBIO_PRESENT` asks the device to show a complete RAM surface. The answer
 * is 0 for a device flip, 1 for the software copy, or a negative error. After
 * a 0 the source is the front surface and must not be modified until a later
 * present replaces it; after a 1 the original framebuffer is live again.
 */
#define FBIO_TAKE 0x4D01u
#define FBIO_GIVE 0x4D02u
#define FBIO_PRESENT 0x4D03u

/*
 * The header of a `write` of `/dev/fb0` and its payload, checked against the
 * picture: 0 when the rectangle is whole and its pixels are in `*pixels`,
 * KERR_INVAL when a field is outside the picture, a dimension is zero, the
 * payload is not exactly `w * h` pixels or the buffer is too short. There is
 * no clipping: a rectangle that leaves the screen is refused, which is what
 * the ADR promises and what keeps a drawing bug visible.
 */
int fb_rect_parse(const struct fb_info *info, const void *buffer, k_size_t size,
                  struct fb_rect *out, const k_u16_t **pixels);

#endif /* MICRO_OS_KERNEL_FB_H */
