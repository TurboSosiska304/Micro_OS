/*
 * Micro OS — the display of the board.
 */
#ifndef MICRO_OS_DRIVERS_DISPLAY_H
#define MICRO_OS_DRIVERS_DISPLAY_H

#include "kernel/types.h"

/* The size of the screen: the panel in its own portrait. The picture a
 * person sees may be the other way round — see drv_display_rotation. */
#define DISPLAY_WIDTH 135
#define DISPLAY_HEIGHT 240

/* Draw a text on the screen. The origin is the top left corner. */
int drv_display_text(int x, int y, const char *text, uint16_t color);

/* Fill the screen with a color. */
int drv_display_clear(uint16_t color);

/* Send the framebuffer to the panel. */
int drv_display_flush(void);

/*
 * The picture as its viewer sees it: the panel turned by quarter turns
 * clockwise (0..3, anything else is taken modulo 4). The panel itself
 * keeps its portrait — the driver turns the drawing, not the glass, so
 * the offset of the visible area never has to change with the turn.
 */
int drv_display_width(void);  /* the width of the picture, 135 or 240 */
int drv_display_height(void); /* the height of the picture, 240 or 135 */
void drv_display_set_rotation(int rotation);
int drv_display_rotation(void);

/*
 * The graphics: written into the framebuffer through the rotation and
 * clipped to the picture, the points outside it are not drawn. Like the
 * text, they are only seen after drv_display_flush().
 */
void drv_display_pixel(int x, int y, uint16_t color);
void drv_display_fill_rect(int x, int y, int w, int h, uint16_t color);
void drv_display_rect(int x, int y, int w, int h, uint16_t color);
void drv_display_line(int x0, int y0, int x1, int y1, uint16_t color);

/* The offset of the visible area of the panel. */
void drv_display_set_offset(int x, int y);
void drv_display_get_offset(int *x, int *y);

#endif /* MICRO_OS_DRIVERS_DISPLAY_H */
