/*
 * Micro OS — the canvas and the frame (ui/draw.h).
 *
 * Every function is a rectangle of pixels and nothing else: the canvas
 * clips, the target refuses, and the caller decides which of the two it is
 * talking to. The title of a window is a row of text drawn into the scratch
 * of the caller and sent as one rectangle of the target, the same way the
 * body of a window is drawn by the program that owns it.
 */
#include "ui/draw.h"

#include <string.h>

#include "kernel/types.h"

void ui_canvas_init(struct ui_canvas *canvas, int width, int height, k_u16_t *pixels)
{
    if (canvas == NULL) {
        return;
    }

    canvas->width = width;
    canvas->height = height;
    canvas->pixels = pixels;
}

static bool canvas_ready(const struct ui_canvas *canvas)
{
    return canvas != NULL && canvas->pixels != NULL && canvas->width > 0 && canvas->height > 0;
}

void ui_canvas_fill(struct ui_canvas *canvas, k_u16_t color)
{
    int index;
    int count;

    if (!canvas_ready(canvas)) {
        return;
    }

    count = canvas->width * canvas->height;

    for (index = 0; index < count; index++) {
        canvas->pixels[index] = color;
    }
}

int ui_canvas_fill_rect(struct ui_canvas *canvas, const struct ui_rect *rect, k_u16_t color)
{
    int top;
    int bottom;
    int left;
    int right;
    int x;
    int y;

    if (!canvas_ready(canvas) || rect == NULL) {
        return -1;
    }

    if (rect->w <= 0 || rect->h <= 0) {
        return 0;
    }

    left = rect->x > 0 ? rect->x : 0;
    top = rect->y > 0 ? rect->y : 0;
    right = rect->x + rect->w < canvas->width ? rect->x + rect->w : canvas->width;
    bottom = rect->y + rect->h < canvas->height ? rect->y + rect->h : canvas->height;

    for (y = top; y < bottom; y++) {
        for (x = left; x < right; x++) {
            canvas->pixels[y * canvas->width + x] = color;
        }
    }

    return 0;
}

int ui_canvas_hline(struct ui_canvas *canvas, int x, int y, int w, k_u16_t color)
{
    struct ui_rect rect;

    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = 1;

    return ui_canvas_fill_rect(canvas, &rect, color);
}

int ui_canvas_vline(struct ui_canvas *canvas, int x, int y, int h, k_u16_t color)
{
    struct ui_rect rect;

    rect.x = x;
    rect.y = y;
    rect.w = 1;
    rect.h = h;

    return ui_canvas_fill_rect(canvas, &rect, color);
}

int ui_canvas_frame(struct ui_canvas *canvas, const struct ui_rect *rect, k_u16_t color)
{
    if (!canvas_ready(canvas) || rect == NULL || rect->w <= 0 || rect->h <= 0) {
        return -1;
    }

    (void)ui_canvas_hline(canvas, rect->x, rect->y, rect->w, color);
    (void)ui_canvas_hline(canvas, rect->x, rect->y + rect->h - 1, rect->w, color);
    (void)ui_canvas_vline(canvas, rect->x, rect->y, rect->h, color);
    (void)ui_canvas_vline(canvas, rect->x + rect->w - 1, rect->y, rect->h, color);

    return 0;
}

int ui_canvas_text(struct ui_canvas *canvas, int x, int y, const char *text, int cells, k_u16_t ink,
                   k_u16_t paper, const struct ui_font *font)
{
    size_t length;
    bool ended = false;
    int index;
    int row;
    int column;

    if (!canvas_ready(canvas) || font == NULL || font->glyph == NULL || font->width <= 0 ||
        font->width > 8 || font->height <= 0 || cells < 0) {
        return -1;
    }

    /* The text is read up to its terminator and not up to `cells`: a buffer
     * of a program holds a line and its NUL, and the bytes after the NUL are
     * not the text of the line. */
    length = text != NULL ? strlen(text) : 0u;

    for (index = 0; index < cells; index++) {
        const k_u8_t *glyph;
        char character = ' ';
        int cell_x = x + index * font->width;

        if (!ended) {
            if (text == NULL || (size_t)index >= length) {
                ended = true;
            } else if (text[index] == '\n') {
                ended = true;
            } else {
                character = text[index];
            }
        }

        glyph = font->glyph(character);
        if (glyph == NULL) {
            return -1;
        }

        /* A cell outside the width paints nothing; a cell partly inside is
         * clipped by the loop of the rows and the columns. */
        if (cell_x + font->width <= 0 || cell_x >= canvas->width) {
            continue;
        }

        for (row = 0; row < font->height; row++) {
            int py = y + row;

            if (py < 0 || py >= canvas->height) {
                continue;
            }

            for (column = 0; column < font->width; column++) {
                int px = cell_x + column;

                if (px < 0 || px >= canvas->width) {
                    continue;
                }

                canvas->pixels[py * canvas->width + px] =
                    (glyph[row] & (k_u8_t)(0x80u >> column)) != 0u ? ink : paper;
            }
        }
    }

    return 0;
}

static bool target_ready(const struct ui_target *target)
{
    return target != NULL && target->fill != NULL && target->blit != NULL;
}

int ui_draw_frame(const struct ui_target *target, const struct ui_window *window,
                  const struct ui_font *font, const struct ui_style *style, bool focused,
                  k_u16_t *scratch)
{
    struct ui_canvas canvas;
    struct ui_rect body;
    struct ui_rect line;
    char title[UI_TITLE_MAX];
    size_t length;
    int rc;

    if (!target_ready(target) || window == NULL || style == NULL || font == NULL ||
        window->rect.w <= 2 * UI_FRAME_MARGIN || window->rect.h < UI_FRAME_TITLE_HEIGHT(font) ||
        font->width <= 0 || font->height <= 0) {
        return -1;
    }

    /* The paper of the whole window: the body and the frame in one fill, so
     * the border lines are drawn over a rectangle nobody else has to clear. */
    rc = target->fill(target->context, &window->rect, style->paper);
    if (rc < 0) {
        return rc;
    }

    /* The border: four lines of one pixel. The focus is a different colour
     * and not a second thickness, so a frame does not move. */
    line.x = window->rect.x;
    line.y = window->rect.y;
    line.w = window->rect.w;
    line.h = 1;
    rc = target->fill(target->context, &line, focused ? style->focus_border : style->border);
    if (rc < 0) {
        return rc;
    }

    line.y = window->rect.y + window->rect.h - 1;
    rc = target->fill(target->context, &line, focused ? style->focus_border : style->border);
    if (rc < 0) {
        return rc;
    }

    line.y = window->rect.y;
    line.w = 1;
    line.h = window->rect.h;
    rc = target->fill(target->context, &line, focused ? style->focus_border : style->border);
    if (rc < 0) {
        return rc;
    }

    line.x = window->rect.x + window->rect.w - 1;
    rc = target->fill(target->context, &line, focused ? style->focus_border : style->border);
    if (rc < 0) {
        return rc;
    }

    if (scratch == NULL) {
        return -1;
    }

    /* The title: the row of the font at the margin of the frame, with the
     * separator under it. The canvas is the width of the window, so the
     * scratch has to hold the whole row. */
    length = strlen(window->title);
    if (length >= sizeof(title)) {
        length = sizeof(title) - 1u;
    }
    memcpy(title, window->title, length);
    title[length] = '\0';

    ui_canvas_init(&canvas, window->rect.w, font->height, scratch);
    ui_canvas_fill(&canvas, style->title_paper);

    rc = ui_canvas_text(&canvas, UI_FRAME_PAD, 0, title, window->rect.w / font->width,
                        style->title_ink, style->title_paper, font);
    if (rc < 0) {
        return rc;
    }

    body.x = window->rect.x;
    body.y = window->rect.y + UI_FRAME_MARGIN;
    body.w = window->rect.w;
    body.h = font->height;
    rc = target->blit(target->context, &body, scratch);
    if (rc < 0) {
        return rc;
    }

    /* The separator under the title: one pixel in its own tone of the border,
     * so the bar reads as a bar and the frame keeps the accent of the focus
     * for itself. */
    line.x = window->rect.x + 1;
    line.y = window->rect.y + UI_FRAME_MARGIN + font->height;
    line.w = window->rect.w - 2;
    line.h = 1;
    rc = target->fill(target->context, &line, style->separator);

    return rc;
}
