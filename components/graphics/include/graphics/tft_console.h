/*
 * Micro OS — the console on the display of the board.
 */
#ifndef MICRO_OS_GRAPHICS_TFT_CONSOLE_H
#define MICRO_OS_GRAPHICS_TFT_CONSOLE_H

#include "kernel/types.h"

/* Prepare the screen and start mirroring the output of the shell. */
void tft_console_start(void);

/* The sink of the output of the shell. */
void tft_console_write(const char *data, k_size_t len);

#endif /* MICRO_OS_GRAPHICS_TFT_CONSOLE_H */
