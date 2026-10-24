/*
 * Micro OS — drawing into a framebuffer that may be turned.
 *
 * The panel of the board is portrait and stays portrait: the driver
 * never touches the controller's rotation, because the offset of the
 * visible area of the glass (a property of the panel) would then have
 * to be different for every quarter turn. The picture is turned here
 * instead — every coordinate a caller gives is a coordinate of the
 * picture as its viewer sees it, and this file maps it onto the pixel
 * of the panel.
 *
 * No hardware, no clock, no locks: plain arithmetic over a pixel
 * buffer, which is why the host tests compile this file themselves and
 * turn test pictures through it.
 */
#ifndef MICRO_OS_DRIVERS_FB_DRAW_H
#define MICRO_OS_DRIVERS_FB_DRAW_H

#include "kernel/types.h"

/*
 * The framebuffer and the way the viewer is looking at it: the pixels
 * are always the panel in its own portrait (width x height), and the
 * rotation says how many quarter turns clockwise the picture is turned
 * for the viewer (0..3; anything else is taken modulo 4).
 */
struct fb_view {
    uint16_t *pixels; /* width * height pixels, the portrait of the panel */
    int width;        /* the physical width (135 on this board) */
    int height;       /* the physical height (240 on this board) */
    int rotation;     /* 0..3 quarter turns clockwise */
};

/* Fill the view. NULL is left alone (the drawing of a view without a
 * picture is a drawing of nothing, and it must not crash). */
void fb_view_init(struct fb_view *view, uint16_t *pixels, int width, int height, int rotation);

/* The size of the picture as the viewer sees it: the sides swap on the
 * quarter turns 1 and 3. */
int fb_view_width(const struct fb_view *view);
int fb_view_height(const struct fb_view *view);

/*
 * The pixel of the panel under the coordinate of the picture: 1 and the
 * coordinates written into px and py when the point is on the picture,
 * and 0 with nothing written when it is outside — every drawing is
 * clipped to the picture it is drawn on.
 */
bool fb_view_map(const struct fb_view *view, int x, int y, int *px, int *py);

/* One pixel of the picture. */
void fb_view_pixel(struct fb_view *view, int x, int y, uint16_t color);

/* A filled rectangle: the part outside the picture is not drawn. */
void fb_view_fill_rect(struct fb_view *view, int x, int y, int w, int h, uint16_t color);

/* The outline of a rectangle, one pixel thick. */
void fb_view_rect(struct fb_view *view, int x, int y, int w, int h, uint16_t color);

/* A straight line between two points of the picture (Bresenham). */
void fb_view_line(struct fb_view *view, int x0, int y0, int x1, int y1, uint16_t color);

#endif /* MICRO_OS_DRIVERS_FB_DRAW_H */
