/*
 * Micro OS — the 8x16 font of the x86-64 console.
 *
 * The port of the board has its 5x7 font in components/font: it is small,
 * uppercase-only and right for a panel of 135x240. The screen of a PC has
 * more room and a person reads a shell there, so the console of this port
 * uses a full ASCII font of 8x16, Spleen 8x16 2.3.0 (BSD-2-Clause; see
 * third_party/spleen). One byte is one row and the most significant bit is
 * the leftmost pixel, the order a VGA font and a PSF font use.
 */
#ifndef MICRO_OS_PORT_X86_64_FONT_H
#define MICRO_OS_PORT_X86_64_FONT_H

#include "kernel/types.h"

#define FONT8_WIDTH 8
#define FONT8_HEIGHT 16

/* The 95 glyphs of the printable ASCII (0x20 to 0x7E), 16 rows each. */
extern const k_u8_t k_font8x16_ascii[95][FONT8_HEIGHT];

/* The glyph of a character, or the one of '?' when it is not printable. */
const k_u8_t *font8x16_glyph(char character);

#endif /* MICRO_OS_PORT_X86_64_FONT_H */
