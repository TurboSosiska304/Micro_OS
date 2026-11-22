/*
 * Micro OS — the picture of the system.
 *
 * The console of the display draws text and boxes, and it must not know
 * what draws them: the panel of the board, a file, a piece of the memory
 * in a test. The renderer is that abstract picture: a width, a height and
 * the five operations the console needs. The driver of the panel registers
 * one at the boot; a board without a display registers none, and the
 * console of the serial port simply has no copy on a screen.
 *
 * This is the Linux "framebuffer console" arrangement reduced to what the
 * board can afford: the console talks to a framebuffer that somebody else
 * owns, not to the controller of a panel.
 */
#ifndef MICRO_OS_KERNEL_RENDER_H
#define MICRO_OS_KERNEL_RENDER_H

#include "kernel/types.h"

struct k_renderer {
    const char *name; /* "st7789v2", "ansi", "memory", ... */

    /* The picture as the viewer sees it: the turn changes these. */
    int (*width)(void *ctx);
    int (*height)(void *ctx);

    /*
     * The size of one cell of the text: the console counts the room of the
     * dialogue in cells, and a renderer that scales the font (a system of
     * /etc, ADR 0021) answers with the cell of its scale. NULL is the 5x7 of
     * the font with the spacing the console has always had.
     */
    int (*cell_width)(void *ctx);
    int (*cell_height)(void *ctx);

    /* Fill the whole picture with a color. */
    void (*clear)(void *ctx, k_u16_t color);

    /* Text at the top left corner of (x, y). */
    void (*text)(void *ctx, int x, int y, const char *text, k_u16_t color);

    /* The graphics the console and the commands use. */
    void (*fill_rect)(void *ctx, int x, int y, int w, int h, k_u16_t color);
    void (*rect)(void *ctx, int x, int y, int w, int h, k_u16_t color);
    void (*line)(void *ctx, int x0, int y0, int x1, int y1, k_u16_t color);

    /* Send what was drawn to the glass. NULL when the drawing is direct. */
    void (*flush)(void *ctx);

    void *ctx;
};

/*
 * Install the picture of the platform; NULL takes it away. The driver of
 * the panel calls it when it comes up, before the console starts.
 */
void k_render_register(const struct k_renderer *renderer);

/* The picture, or NULL when the board has no display. */
const struct k_renderer *k_render_get(void);

#endif /* MICRO_OS_KERNEL_RENDER_H */
