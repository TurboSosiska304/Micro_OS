/*
 * Micro OS — the picture of the x86-64 machine: a linear framebuffer.
 *
 * ADR 0023 gives this port the screen of the board in another shape: a
 * framebuffer of 1280 by 800 pixels in RGB565, which is the format the
 * renderer of the kernel speaks (kernel/render.h), so the font, the boxes and
 * the console draw into it without a translation of their colours. The
 * renderer is registered at the boot like the panel of the board registers
 * its own, and the console of the port draws through it: this file is one
 * more `struct k_renderer` and not a second drawing library.
 *
 * The console keeps a grid of characters, one per cell of the 5x7 font at
 * twice its size - 10 by 14 pixels, 128 columns by 57 rows - and asks for the
 * one cell that changed. A character is a glyph blitted into the memory, a
 * scroll is a move of the whole picture up by one cell, and the colours are
 * two numbers of RGB565 that the `console` system of /etc chooses (ADR 0021).
 *
 * A pixel is one store into the memory the card shows. There is no shadow
 * frame, no double buffer and no flush: the window of QEMU reads the memory
 * it is given, so the picture is the framebuffer and a flush would copy it
 * onto itself.
 */
#include "port_x86_64_screen.h"

#include <stdio.h>
#include <string.h>

#include "graphics/font.h"
#include "kernel/log.h"
#include "kernel/render.h"
#include "port_x86_64_console.h"

/* The font at twice its size: the cell of ADR 0023. */
#define FB_SCALE 2
#define FB_CELL_WIDTH (FONT_WIDTH * FB_SCALE)   /* 10 pixels */
#define FB_CELL_HEIGHT (FONT_HEIGHT * FB_SCALE) /* 14 pixels */

/* The biggest grid the port draws: the mode of ADR 0023 and some room, so a
 * mode of another shape is cut to the array instead of overrunning it. */
#define FB_COLUMNS_MAX 160
#define FB_ROWS_MAX 60

/* The colours of the two themes, the pair the VGA calls grey on black and
 * black on light grey. 0xAD55 is the RGB565 of the light grey of the VGA. */
#define FB_GREY 0xAD55u

static volatile k_u16_t *g_fb;
static int g_width;
static int g_height;
static int g_stride; /* pixels of a line, the pitch of the mode in pixels */

static int g_columns;
static int g_rows;
static char g_cells[FB_ROWS_MAX][FB_COLUMNS_MAX];

static k_u16_t g_ink;
static k_u16_t g_paper;

static int g_cursor_row = -1;
static int g_cursor_column = -1;

static char g_name[64];

/* --- the memory of the picture --------------------------------------------- */

static void pixel_put(int x, int y, k_u16_t color)
{
    if (x < 0 || x >= g_width || y < 0 || y >= g_height) {
        return;
    }

    g_fb[y * g_stride + x] = color;
}

static void area_fill(int x, int y, int w, int h, k_u16_t color)
{
    int row;
    int column;

    for (row = 0; row < h; row++) {
        for (column = 0; column < w; column++) {
            pixel_put(x + column, y + row, color);
        }
    }
}

/* One glyph of the font at the scale of the screen: the lowest bit of a
 * column of the font is the top row of the letter, which is the order the
 * board has always used. */
static void glyph_draw(int x, int y, char character, k_u16_t color)
{
    const k_u8_t *glyph = font_glyph(character);
    int column;
    int row;
    int sy;
    int sx;

    for (column = 0; column < FONT_WIDTH; column++) {
        k_u8_t bits = glyph[column];

        for (row = 0; row < FONT_HEIGHT; row++) {
            if ((bits & (k_u8_t)(1u << row)) == 0u) {
                continue;
            }

            for (sy = 0; sy < FB_SCALE; sy++) {
                for (sx = 0; sx < FB_SCALE; sx++) {
                    pixel_put(x + column * FB_SCALE + sx, y + row * FB_SCALE + sy, color);
                }
            }
        }
    }
}

/* --- the renderer of the kernel -------------------------------------------- */

static int render_width(void *ctx)
{
    K_UNUSED(ctx);

    return g_width;
}

static int render_height(void *ctx)
{
    K_UNUSED(ctx);

    return g_height;
}

static int render_cell_width(void *ctx)
{
    K_UNUSED(ctx);

    return FB_CELL_WIDTH;
}

static int render_cell_height(void *ctx)
{
    K_UNUSED(ctx);

    return FB_CELL_HEIGHT;
}

static void render_clear(void *ctx, k_u16_t color)
{
    K_UNUSED(ctx);

    area_fill(0, 0, g_width, g_height, color);
}

static void render_text(void *ctx, int x, int y, const char *text, k_u16_t color)
{
    int index;

    K_UNUSED(ctx);

    if (text == NULL) {
        return;
    }

    for (index = 0; text[index] != '\0'; index++) {
        glyph_draw(x + index * FB_CELL_WIDTH, y, text[index], color);
    }
}

static void render_fill_rect(void *ctx, int x, int y, int w, int h, k_u16_t color)
{
    K_UNUSED(ctx);

    area_fill(x, y, w, h, color);
}

static void render_rect(void *ctx, int x, int y, int w, int h, k_u16_t color)
{
    K_UNUSED(ctx);

    area_fill(x, y, w, 1, color);
    area_fill(x, y + h - 1, w, 1, color);
    area_fill(x, y, 1, h, color);
    area_fill(x + w - 1, y, 1, h, color);
}

static void render_line(void *ctx, int x0, int y0, int x1, int y1, k_u16_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx - dy;

    K_UNUSED(ctx);

    /* The line of Bresenham: the same the panel of the board draws, because
     * a command that draws a line has to draw the same line on both. */
    for (;;) {
        pixel_put(x0, y0, color);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        if (2 * error >= -dy) {
            error -= dy;
            x0 += sx;
        }

        if (2 * error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

/* The picture does not need a flush: the memory is the glass. */
static const struct k_renderer s_renderer = {
    .name = "bochs-vbe",
    .width = render_width,
    .height = render_height,
    .cell_width = render_cell_width,
    .cell_height = render_cell_height,
    .clear = render_clear,
    .text = render_text,
    .fill_rect = render_fill_rect,
    .rect = render_rect,
    .line = render_line,
    .flush = NULL,
    .ctx = NULL,
};

/* --- the grid of cells the console draws ----------------------------------- */

/* One cell as the glass shows it: the paper, then the glyph, or the reverse
 * when the cursor is on it. */
static void cell_paint(int row, int column, bool inverted)
{
    int x = column * FB_CELL_WIDTH;
    int y = row * FB_CELL_HEIGHT;
    char character;

    if (row < 0 || row >= g_rows || column < 0 || column >= g_columns) {
        return;
    }

    character = g_cells[row][column];

    if (inverted) {
        render_fill_rect(NULL, x, y, FB_CELL_WIDTH, FB_CELL_HEIGHT, g_ink);
        if (character != ' ') {
            glyph_draw(x, y, character, g_paper);
        }
        return;
    }

    render_fill_rect(NULL, x, y, FB_CELL_WIDTH, FB_CELL_HEIGHT, g_paper);
    if (character != ' ') {
        glyph_draw(x, y, character, g_ink);
    }
}

static void fb_clear(void)
{
    memset(g_cells, ' ', sizeof(g_cells));
    render_clear(NULL, g_paper);

    g_cursor_row = -1;
    g_cursor_column = -1;
}

static void fb_put(int row, int column, char character)
{
    if (row < 0 || row >= g_rows || column < 0 || column >= g_columns) {
        return;
    }

    g_cells[row][column] = character;
    cell_paint(row, column, false);
}

/*
 * A row up: the picture moves, and the characters move with it. The memory of
 * the picture is one block, so the pixels are one move of the rows that stay;
 * the grid is the same move of a row of characters.
 */
static void fb_scroll(void)
{
    int line = FB_CELL_HEIGHT * g_stride; /* pixels of one cell row */

    memmove((void *)(uintptr_t)g_fb, (const void *)(uintptr_t)(g_fb + line),
            (k_size_t)(g_height - FB_CELL_HEIGHT) * (k_size_t)g_stride * sizeof(k_u16_t));
    area_fill(0, g_height - FB_CELL_HEIGHT, g_width, FB_CELL_HEIGHT, g_paper);

    memmove(g_cells[0], g_cells[1], (k_size_t)(FB_ROWS_MAX - 1) * FB_COLUMNS_MAX);
    memset(g_cells[g_rows - 1], ' ', FB_COLUMNS_MAX);

    /*
     * The cursor is the one cell the memory does not show as the grid says:
     * the block moved up with the picture, and the cell it left has to read
     * as its character again. The console names the new position after the
     * scroll; the old one is this file's to put right.
     */
    if (g_cursor_row > 0) {
        cell_paint(g_cursor_row - 1, g_cursor_column, false);
    }

    g_cursor_row = -1;
    g_cursor_column = -1;
}

static void fb_cursor(int row, int column)
{
    if (row == g_cursor_row && column == g_cursor_column) {
        return;
    }

    /* The cell the cursor leaves reads as the character it holds again. */
    if (g_cursor_row >= 0) {
        cell_paint(g_cursor_row, g_cursor_column, false);
    }

    g_cursor_row = row;
    g_cursor_column = column;

    cell_paint(row, column, true);
}

/* The theme of `console`: the pair repaints the grid and the picture. */
static void fb_colors(bool light, bool invert)
{
    int row;
    int column;

    g_ink = light ? 0x0000u : FB_GREY;
    g_paper = light ? FB_GREY : 0x0000u;

    if (invert) {
        k_u16_t swap = g_ink;

        g_ink = g_paper;
        g_paper = swap;
    }

    render_clear(NULL, g_paper);

    for (row = 0; row < g_rows; row++) {
        for (column = 0; column < g_columns; column++) {
            if (g_cells[row][column] != ' ') {
                cell_paint(row, column, false);
            }
        }
    }

    if (g_cursor_row >= 0) {
        cell_paint(g_cursor_row, g_cursor_column, true);
    }
}

/*
 * The whole picture again: the paper, every cell of the grid, the cursor.
 * Every byte of the window is written, so a display that keeps the picture it
 * drew last sees a complete frame instead of the part of it that changed -
 * which is the point at the boot, when the frame before this one was the text
 * mode of the firmware.
 */
static void fb_redraw(void)
{
    int row;
    int column;

    if (g_fb == NULL) {
        return;
    }

    render_clear(NULL, g_paper);

    for (row = 0; row < g_rows; row++) {
        for (column = 0; column < g_columns; column++) {
            if (g_cells[row][column] != ' ' || (row == g_cursor_row && column == g_cursor_column)) {
                cell_paint(row, column, row == g_cursor_row && column == g_cursor_column);
            }
        }
    }
}

static struct port_x86_64_screen s_screen = {
    .name = g_name,
    .columns = 0, /* the mode fills these in */
    .rows = 0,
    .clear = fb_clear,
    .put = fb_put,
    .scroll = fb_scroll,
    .cursor = fb_cursor,
    .colors = fb_colors,
    .redraw = fb_redraw,
};

const struct port_x86_64_screen *port_x86_64_fb_screen(const struct port_x86_64_screen_mode *mode,
                                                       void *base)
{
    if (mode == NULL || base == NULL || mode->bpp != PORT_X86_64_SCREEN_BPP || mode->width == 0u ||
        mode->height == 0u || mode->pitch < (k_u32_t)mode->width * (mode->bpp / 8u)) {
        return NULL;
    }

    g_fb = (volatile k_u16_t *)base;
    g_width = mode->width;
    g_height = mode->height;
    g_stride = mode->pitch / 2;

    g_columns = g_width / FB_CELL_WIDTH;
    g_rows = g_height / FB_CELL_HEIGHT;

    if (g_columns > FB_COLUMNS_MAX) {
        g_columns = FB_COLUMNS_MAX;
    }
    if (g_rows > FB_ROWS_MAX) {
        g_rows = FB_ROWS_MAX;
    }

    if (g_columns < 1 || g_rows < 1) {
        kerror("screen: the picture of %ux%u leaves no room for a cell", g_width, g_height);
        return NULL;
    }

    /* The theme of the boot: the pair of the dark console, before /etc has
     * been read. The applier of `console` repaints the screen with the pair a
     * person wrote, and the boot's own lines are then read in it. */
    g_ink = FB_GREY;
    g_paper = 0x0000u;

    snprintf(g_name, sizeof(g_name), "the framebuffer of %ux%u, %u bpp, %dx%d cells", g_width,
             g_height, (unsigned)mode->bpp, g_columns, g_rows);

    s_screen.columns = g_columns;
    s_screen.rows = g_rows;

    /* The picture of the machine is the one the console of the kernel draws
     * on when a command asks for it (kernel/render.h). */
    k_render_register(&s_renderer);

    fb_clear();

    return &s_screen;
}
