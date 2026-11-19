/*
 * Micro OS — the picture of the host: a screen made of characters.
 *
 * The board has a 135x240 panel and a renderer of its own in the driver. The
 * host has neither, but the console of the display is portable and draws
 * through kernel/render.h, so a renderer that keeps a frame in the memory
 * makes that console visible on a laptop.
 *
 * The frame is 80 columns of 24 rows of a 6x8 cell - a terminal, not a
 * panel. The coordinates of the interface are pixels, so every drawing call
 * is reduced to a cell. The file is written only when $MICROOS_SCREEN is
 * set: the smoke test of the simulator must not find a screen file that
 * nobody asked for, and a board without a display would register nothing.
 */
#include "host_stubs.h"

#include <stdio.h>

#include "kernel/render.h"

#define HOST_COLUMNS 80
#define HOST_ROWS 24

/* The cell of the text: the font of the board is 5 wide and 7 tall. */
#define HOST_CELL_WIDTH 6
#define HOST_CELL_HEIGHT 9

static char s_frame[HOST_ROWS][HOST_COLUMNS];

static void frame_put(int column, int row, char glyph)
{
    if (column < 0 || column >= HOST_COLUMNS || row < 0 || row >= HOST_ROWS) {
        return;
    }

    s_frame[row][column] = glyph;
}

static int screen_width(void *ctx)
{
    (void)ctx;

    return HOST_COLUMNS * HOST_CELL_WIDTH;
}

static int screen_height(void *ctx)
{
    (void)ctx;

    return HOST_ROWS * HOST_CELL_HEIGHT;
}

static void screen_clear(void *ctx, k_u16_t color)
{
    (void)ctx;
    (void)color;

    for (int row = 0; row < HOST_ROWS; row++) {
        for (int column = 0; column < HOST_COLUMNS; column++) {
            s_frame[row][column] = ' ';
        }
    }
}

static void screen_text(void *ctx, int x, int y, const char *text, k_u16_t color)
{
    int column = x / HOST_CELL_WIDTH;
    int row = y / HOST_CELL_HEIGHT;

    (void)ctx;
    (void)color;

    if (text == NULL) {
        return;
    }

    for (; *text != '\0'; text++) {
        char glyph = *text;

        if (glyph == '\n') {
            column = x / HOST_CELL_WIDTH;
            row++;
            continue;
        }

        if (glyph == '\r') {
            column = x / HOST_CELL_WIDTH;
            continue;
        }

        /* The font knows the printable ASCII; anything else becomes '?'. */
        if (glyph < ' ' || glyph > '~') {
            glyph = '?';
        }

        frame_put(column++, row, glyph);
    }
}

static void screen_fill_rect(void *ctx, int x, int y, int w, int h, k_u16_t color)
{
    int first_column;
    int last_column;
    int first_row;
    int last_row;

    (void)ctx;
    (void)color;

    if (w <= 0 || h <= 0) {
        return;
    }

    first_column = x / HOST_CELL_WIDTH;
    last_column = (x + w - 1) / HOST_CELL_WIDTH;
    first_row = y / HOST_CELL_HEIGHT;
    last_row = (y + h - 1) / HOST_CELL_HEIGHT;

    for (int row = first_row; row <= last_row; row++) {
        for (int column = first_column; column <= last_column; column++) {
            frame_put(column, row, '#');
        }
    }
}

static void screen_rect(void *ctx, int x, int y, int w, int h, k_u16_t color)
{
    int first_column;
    int last_column;
    int first_row;
    int last_row;

    (void)ctx;
    (void)color;

    if (w <= 0 || h <= 0) {
        return;
    }

    first_column = x / HOST_CELL_WIDTH;
    last_column = (x + w - 1) / HOST_CELL_WIDTH;
    first_row = y / HOST_CELL_HEIGHT;
    last_row = (y + h - 1) / HOST_CELL_HEIGHT;

    for (int column = first_column; column <= last_column; column++) {
        frame_put(column, first_row, '#');
        frame_put(column, last_row, '#');
    }

    for (int row = first_row; row <= last_row; row++) {
        frame_put(first_column, row, '#');
        frame_put(last_column, row, '#');
    }
}

/* Bresenham over the cells of the frame: a line is thick on a terminal. */
static void screen_line(void *ctx, int x0, int y0, int x1, int y1, k_u16_t color)
{
    int column = x0 / HOST_CELL_WIDTH;
    int row = y0 / HOST_CELL_HEIGHT;
    int last_column = x1 / HOST_CELL_WIDTH;
    int last_row = y1 / HOST_CELL_HEIGHT;
    int dx = last_column > column ? last_column - column : column - last_column;
    int dy = last_row > row ? last_row - row : row - last_row;
    int step_x = column < last_column ? 1 : -1;
    int step_y = row < last_row ? 1 : -1;
    int error = dx - dy;

    (void)ctx;
    (void)color;

    for (;;) {
        frame_put(column, row, '#');

        if (column == last_column && row == last_row) {
            break;
        }

        if (error * 2 > -dy) {
            error -= dy;
            column += step_x;
        }
        if (error * 2 < dx) {
            error += dx;
            row += step_y;
        }
    }
}

static void screen_flush(void *ctx)
{
    const char *path = host_screen_path();
    FILE *screen;

    (void)ctx;

    if (path == NULL) {
        return;
    }

    screen = fopen(path, "w");
    if (screen == NULL) {
        return;
    }

    for (int row = 0; row < HOST_ROWS; row++) {
        fwrite(s_frame[row], 1, HOST_COLUMNS, screen);
        fputc('\n', screen);
    }

    fclose(screen);
}

static const struct k_renderer s_renderer = {
    .name = "host-screen",
    .width = screen_width,
    .height = screen_height,
    .clear = screen_clear,
    .text = screen_text,
    .fill_rect = screen_fill_rect,
    .rect = screen_rect,
    .line = screen_line,
    .flush = screen_flush,
    .ctx = NULL,
};

void host_render_register(void)
{
    /* No file screen, no picture: the console of the display stays silent. */
    if (host_screen_path() != NULL) {
        screen_clear(NULL, 0);
        k_render_register(&s_renderer);
    }
}
