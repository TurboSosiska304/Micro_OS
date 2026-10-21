/*
 * Micro OS — the display of the board.
 */
#ifndef MICRO_OS_DRIVERS_DISPLAY_H
#define MICRO_OS_DRIVERS_DISPLAY_H

#include "kernel/types.h"

/* The size of the screen. */
#define DISPLAY_WIDTH 135
#define DISPLAY_HEIGHT 240

/* Draw a text on the screen. The origin is the top left corner. */
int drv_display_text(int x, int y, const char *text, uint16_t color);

/* Fill the screen with a color. */
int drv_display_clear(uint16_t color);

/* Send the framebuffer to the panel. */
int drv_display_flush(void);

/* The offset of the visible area of the panel. */
void drv_display_set_offset(int x, int y);
void drv_display_get_offset(int *x, int *y);

#endif /* MICRO_OS_DRIVERS_DISPLAY_H */
