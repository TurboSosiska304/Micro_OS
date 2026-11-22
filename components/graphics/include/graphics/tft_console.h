/*
 * Micro OS — the console on the display of the board.
 */
#ifndef MICRO_OS_GRAPHICS_TFT_CONSOLE_H
#define MICRO_OS_GRAPHICS_TFT_CONSOLE_H

#include "kernel/types.h"

/* Prepare the screen and start mirroring the output of the shell. */
void tft_console_start(void);

/*
 * The colours of the dialogue: the background, the text and the line that is
 * being written. A system of /etc sets them before the console starts, and a
 * console that is already drawing repaints itself (ADR 0021).
 */
void tft_console_set_colors(k_u16_t background, k_u16_t foreground, k_u16_t current);

/*
 * The most columns and rows the dialogue may use, 0 for the whole picture.
 * They are a cap on the room the panel has at the size of the font, so a
 * person can keep the dialogue narrow on a wide screen.
 */
void tft_console_set_cells(int columns, int rows);

/*
 * Draw the dialogue again. The console notices a turned picture by
 * itself; a caller that changes what is on the glass behind its back says
 * so here.
 */
void tft_console_redraw(void);

/* The sink of the output of the shell. */
void tft_console_write(const char *data, k_size_t len);

#endif /* MICRO_OS_GRAPHICS_TFT_CONSOLE_H */
