/*
 * Micro OS — the console on the display of the board.
 */
#ifndef MICRO_OS_GRAPHICS_TFT_CONSOLE_H
#define MICRO_OS_GRAPHICS_TFT_CONSOLE_H

#include "kernel/types.h"

/* Prepare the screen and start mirroring the output of the shell. */
void tft_console_start(void);

/*
 * Draw the dialogue again. The console notices a turned picture by
 * itself; a caller that changes what is on the glass behind its back says
 * so here.
 */
void tft_console_redraw(void);

/* The sink of the output of the shell. */
void tft_console_write(const char *data, k_size_t len);

#endif /* MICRO_OS_GRAPHICS_TFT_CONSOLE_H */
