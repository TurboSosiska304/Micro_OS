/*
 * Micro OS — the font of the display.
 */
#ifndef MICRO_OS_GRAPHICS_FONT_H
#define MICRO_OS_GRAPHICS_FONT_H

#include <stdint.h>

#define FONT_WIDTH 5
#define FONT_HEIGHT 7

/* The glyph of a character: five columns, the lowest bit is the top row. */
const uint8_t *font_glyph(char c);

#endif /* MICRO_OS_GRAPHICS_FONT_H */
