/*
 * Micro OS — the picture of the windows (ADR 0030).
 *
 * The model of `ui/window.h` says where a window is; this file says what is
 * inside it. The picture is a target the caller gives: two functions, one
 * that fills a rectangle with a colour and one that copies pixels into it.
 * On the machine those two functions write `/dev/fb0`; in a test they write
 * an array of the host, which is how a frame is checked without a screen.
 *
 * The drawing itself is a canvas: an array of pixels of the width and the
 * height of the rectangle the caller is about to send, and functions that
 * fill it, outline it and lay out a line of text in it. A program composes a
 * row of text once and sends it as one rectangle, and the library never asks
 * how big a write of the device may be.
 *
 * The font is the caller's too: a font is a cell size and a function from a
 * character to the rows of its bitmap, so the console's 8x16 glyphs, a font
 * of a test and a font of another machine are the same to this file.
 */
#ifndef MICRO_OS_UI_DRAW_H
#define MICRO_OS_UI_DRAW_H

#include <stdbool.h>

#include "kernel/types.h"
#include "ui/window.h"

/*
 * A font of the picture: the cell and the glyph. One byte is one row of the
 * cell, the most significant bit is the leftmost pixel, and the glyph has
 * `height` rows (the shape of a VGA font and of `port_x86_64_font.h`). The
 * width is at most eight - one byte a row is what a glyph is.
 *
 * `scale` is the size of the picture of one pixel of the glyph, and 0 or 1 is
 * the font at its own size. It is what gives a machine with one table of
 * glyphs a second size of its text (docs/desktop.md, ADR 0054): the terminal
 * of the desktop draws the cells of the console at twice their side, which is
 * a 16x32 cell out of the 8x16 rows and not a second table of glyphs.
 */
struct ui_font {
    int width;
    int height;
    int scale;
    const k_u8_t *(*glyph)(char character);
};

/* The side of one cell of the picture, the scale of the font included. */
int ui_font_cell_width(const struct ui_font *font);
int ui_font_cell_height(const struct ui_font *font);

/* A rectangle of pixels of the caller, one row after another. */
struct ui_canvas {
    int width;
    int height;
    k_u16_t *pixels;
};

/*
 * The picture a program draws in. Both functions take a rectangle that is
 * whole inside the picture of the target and answer 0, or a negative number
 * when the device refused the rectangle.
 */
struct ui_target {
    void *context;
    int (*fill)(void *context, const struct ui_rect *rect, k_u16_t color);
    int (*blit)(void *context, const struct ui_rect *rect, const k_u16_t *pixels);
};

/* The colours of the frame of a window: the paper of the body, the ink of a
 * title, the border of a window that does not have the keys and the border of
 * the one that does. `separator` is the line under the title, one pixel in a
 * tone of the border, which is what divides the bar from the body without a
 * second rectangle. */
struct ui_style {
    k_u16_t paper;
    k_u16_t ink;
    k_u16_t border;
    k_u16_t focus_border;
    k_u16_t title_paper;
    k_u16_t title_ink;
    k_u16_t separator;
};

/* The place of the title inside the frame: four pixels of air above the row of
 * the font, six pixels at either side of it, a pixel of separator under it,
 * and the body starts below. The air is the difference between a bar a person
 * reads and a bar a terminal draws: a glyph that touches the border is a
 * character of a text screen, not the title of a window. */
#define UI_FRAME_MARGIN 4
#define UI_FRAME_PAD 6
#define UI_FRAME_TITLE_HEIGHT(font) (UI_FRAME_MARGIN + ui_font_cell_height(font) + 1)

/* --- the canvas ---------------------------------------------------------- */

void ui_canvas_init(struct ui_canvas *canvas, int width, int height, k_u16_t *pixels);

/* The whole canvas in one colour. */
void ui_canvas_fill(struct ui_canvas *canvas, k_u16_t color);

/* A rectangle of one colour, clipped to the canvas: a part of a rectangle that
 * leaves it is drawn, which is not the rule of the device (that one refuses
 * the rectangle, ADR 0028) but is the rule of a drawing that composes. */
int ui_canvas_fill_rect(struct ui_canvas *canvas, const struct ui_rect *rect, k_u16_t color);

/* A line of pixels inside the canvas, clipped the same way. */
int ui_canvas_hline(struct ui_canvas *canvas, int x, int y, int w, k_u16_t color);
int ui_canvas_vline(struct ui_canvas *canvas, int x, int y, int h, k_u16_t color);

/* The outline of a rectangle: four lines of one pixel. */
int ui_canvas_frame(struct ui_canvas *canvas, const struct ui_rect *rect, k_u16_t color);

/*
 * A line of text: `cells` cells from (x, y), each one the glyph of `text` in
 * the ink on the paper. A character past the end of the text, or a newline,
 * is a cell of the paper, so a shorter line clears the tail of a longer one
 * and two lines of one window never share a pixel of the wrong letter.
 */
int ui_canvas_text(struct ui_canvas *canvas, int x, int y, const char *text, int cells, k_u16_t ink,
                   k_u16_t paper, const struct ui_font *font);

/* --- the frame of a window ----------------------------------------------- */

/*
 * The paper, the border and the title of a window, into the target. The
 * scratch is the canvas of the title row and has to hold `rect.w * font->height`
 * pixels; the caller owns it because a frame of the whole screen is a row of
 * 1280 cells and the library allocates nothing (a program's arena is its own).
 *
 * `focused` picks the border of the focus and draws the title in the ink of
 * the title; a window that does not have the keys keeps the plain border.
 * The body of the window is the caller's: it starts at
 * `y + UI_FRAME_TITLE_HEIGHT(font)` and ends at the last pixel of the rect.
 */
int ui_draw_frame(const struct ui_target *target, const struct ui_window *window,
                  const struct ui_font *font, const struct ui_style *style, bool focused,
                  k_u16_t *scratch);

#endif /* MICRO_OS_UI_DRAW_H */
