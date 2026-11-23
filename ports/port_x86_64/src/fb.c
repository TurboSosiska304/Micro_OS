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
 * The console keeps a grid of characters, one per cell of the font of the PC
 * - Spleen 8x16, see port_x86_64_font.h - so a line of 1280 pixels holds 160
 * columns and the picture holds 50 rows. The gap between two lines is a key
 * of the `font` system of /etc (`line_gap`, 0, 2 or 4 pixels): the height of
 * a row of cells is 16 plus the gap, and a picture of 800 lines then holds
 * 50, 44 or 40 of them. A character is a glyph blitted into the memory, a
 * scroll is a move of the whole picture up by one row of cells, and the
 * colours come from two places that are both attributes of a cell: the pair
 * of the `console` system of /etc (ADR 0021) for the plain text, and the
 * sixteen of the CGA palette for the words a command colours through the SGR
 * sequences (kernel/ansi.h). Every cell carries its own (paper << 4) | ink,
 * so a red warning and a grey line are one grid and one scroll.
 *
 * A pixel is one store into the memory the card shows. There is no shadow
 * frame, no double buffer and no flush: the window of QEMU reads the memory
 * it is given, so the picture is the framebuffer and a flush would copy it
 * onto itself.
 */
#include "port_x86_64_screen.h"

#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/fb.h"
#include "kernel/log.h"
#include "kernel/render.h"
#include "port_x86_64_console.h"
#include "port_x86_64_font.h"
#include "settings/settings.h"

/* The cell of ADR 0023: one glyph of the 8x16 font of the PC, and the step of
 * a line is its height plus the gap of /etc/font. */
#define FB_CELL_WIDTH FONT8_WIDTH   /* 8 pixels */
#define FB_CELL_HEIGHT FONT8_HEIGHT /* 16 pixels */
#define FB_LINE_GAP_MAX 4

/* The biggest grid the port draws: the mode of ADR 0023 (160 columns of 8
 * pixels) and some room, so a mode of another shape is cut to the array
 * instead of overrunning it. */
#define FB_COLUMNS_MAX 170
#define FB_ROWS_MAX 60

/* The colours of the two themes, the pair the VGA calls grey on black and
 * black on light grey. 0xAD55 is the RGB565 of the light grey of the VGA. */
#define FB_GREY 0xAD55u
#define FB_BLACK 0x0000u

/* The attribute of the plain text before /etc is read: light grey on black,
 * the pair a VGA starts in. */
#define FB_ATTR_BOOT PORT_X86_64_ATTR(7, 0)

/*
 * The sixteen colours of the CGA/VGA palette in RGB565. The CGA numbers the
 * two bits of red, green and blue per channel of an RGBI signal; the values
 * below are the same eight colours and their bright half, widened to the
 * five-six-five the renderer of the kernel speaks. Black, blue, green, cyan,
 * red, magenta, brown (the "amber" of a terminal), light grey, then the
 * bright eight. The text mode has these colours by definition, so the two
 * screens of the port paint the same red for the same attribute.
 */
const k_u16_t port_x86_64_palette[16] = {
    0x0000u, /* 0  black */
    0x0015u, /* 1  blue        (0, 0, 170) */
    0x0540u, /* 2  green       (0, 170, 0) */
    0x0555u, /* 3  cyan        (0, 170, 170) */
    0xA800u, /* 4  red         (170, 0, 0) */
    0xA815u, /* 5  magenta     (170, 0, 170) */
    0xAD40u, /* 6  brown/amber (170, 85, 0) */
    0xAD55u, /* 7  light grey  (170, 170, 170) */
    0x52AAu, /* 8  dark grey   (85, 85, 85) */
    0x52BFu, /* 9  bright blue (85, 85, 255) */
    0x57EAu, /* 10 bright green (85, 255, 85) */
    0x57FFu, /* 11 bright cyan (85, 255, 255) */
    0xFAAAu, /* 12 bright red  (255, 85, 85) */
    0xFABFu, /* 13 bright magenta (255, 85, 255) */
    0xFFEAu, /* 14 yellow      (255, 255, 85) */
    0xFFFFu, /* 15 white */
};

/* One cell of the grid: the letter and the attribute of its colours. */
struct fb_cell {
    char character;
    k_u8_t attr;
};

static volatile k_u16_t *g_fb;
static int g_width;
static int g_height;
static int g_stride; /* pixels of a line, the pitch of the mode in pixels */

static int g_columns;
static int g_rows;
static int g_pitch = FB_CELL_HEIGHT; /* the step of a line: the cell and the gap */
static struct fb_cell g_cells[FB_ROWS_MAX][FB_COLUMNS_MAX];

/* The pair of the console: the ink and the paper of the plain text, and the
 * attribute that names them. A cell whose attribute is this pair is painted
 * with these two numbers, which is how a colour of 0xRRRR that is not one of
 * the sixteen still reaches the picture. */
static k_u16_t g_ink;
static k_u16_t g_paper;
static k_u8_t g_theme_attr = FB_ATTR_BOOT;

static int g_cursor_row = -1;
static int g_cursor_column = -1;

/*
 * The console has been taken by a program (ADR 0028): the kernel keeps its
 * grid and its cursor, but it writes no pixel until the screen comes back.
 * The program's picture is then the whole glass; FBIO_GIVE repaints the grid
 * the kernel kept and no line of the log is lost.
 */
static bool g_console_taken;

static char g_name[64];

/* The inverse of an attribute: the paper and the ink traded, which is the
 * cursor and the SGR 7 of a terminal. */
static k_u8_t attr_reverse(k_u8_t attr)
{
    return PORT_X86_64_ATTR(PORT_X86_64_ATTR_BG(attr), PORT_X86_64_ATTR_FG(attr));
}

/*
 * The ink and the paper of one cell. An attribute of the pair of the console
 * is painted with the two numbers of /etc - a custom 0xRRRR included - and
 * its inverse is the same two numbers traded; every other attribute names two
 * of the sixteen colours.
 */
static void attr_colors(k_u8_t attr, bool inverted, k_u16_t *ink, k_u16_t *paper)
{
    if (attr == g_theme_attr) {
        *ink = g_ink;
        *paper = g_paper;
    } else if (attr == attr_reverse(g_theme_attr)) {
        *ink = g_paper;
        *paper = g_ink;
    } else {
        *ink = port_x86_64_palette[PORT_X86_64_ATTR_FG(attr)];
        *paper = port_x86_64_palette[PORT_X86_64_ATTR_BG(attr)];
    }

    if (inverted) {
        k_u16_t swap = *ink;

        *ink = *paper;
        *paper = swap;
    }
}

k_u8_t port_x86_64_color_nearest(k_u16_t rgb)
{
    int red = (rgb >> 11) & 0x1F;
    int green = (rgb >> 5) & 0x3F;
    int blue = rgb & 0x1F;
    long best = -1;
    k_u8_t index = 0;
    int i;

    for (i = 0; i < 16; i++) {
        int dr = red - (int)((port_x86_64_palette[i] >> 11) & 0x1F);
        int dg = green - (int)((port_x86_64_palette[i] >> 5) & 0x3F);
        int db = blue - (int)(port_x86_64_palette[i] & 0x1F);
        long distance = (long)dr * dr + (long)dg * dg + (long)db * db;

        if (best < 0 || distance < best) {
            best = distance;
            index = (k_u8_t)i;
        }
    }

    return index;
}

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

/* One glyph of the font of the PC: one byte is one row of the letter and the
 * most significant bit is the leftmost pixel, the order a VGA font and a PSF
 * font use. The cell is the glyph - 8 by 16 pixels - and there is no scale:
 * the font of this screen is already the size a person reads. */
static void glyph_draw(int x, int y, char character, k_u16_t color)
{
    const k_u8_t *glyph = font8x16_glyph(character);
    int column;
    int row;

    for (row = 0; row < FONT8_HEIGHT; row++) {
        k_u8_t bits = glyph[row];

        if (bits == 0u) {
            continue;
        }

        for (column = 0; column < FONT8_WIDTH; column++) {
            if ((bits & (k_u8_t)(0x80u >> column)) != 0u) {
                pixel_put(x + column, y + row, color);
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

/* The step of a line, not the height of the glyph: the console and a command
 * lay lines out with the pitch of the screen, which the `line_gap` of
 * /etc/font widens. */
static int render_cell_height(void *ctx)
{
    K_UNUSED(ctx);

    return g_pitch;
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

/* One cell as the glass shows it: the paper of its colours, then the glyph in
 * the ink - or the two traded when the cursor is on it. The rows below the
 * box are the gap of the line and belong to nobody: a cell paints its own
 * eight by sixteen. */
static void cell_paint(int row, int column, bool inverted)
{
    int x = column * FB_CELL_WIDTH;
    int y = row * g_pitch;
    k_u16_t ink;
    k_u16_t paper;
    struct fb_cell *cell;

    if (row < 0 || row >= g_rows || column < 0 || column >= g_columns) {
        return;
    }

    cell = &g_cells[row][column];
    attr_colors(cell->attr, inverted, &ink, &paper);

    render_fill_rect(NULL, x, y, FB_CELL_WIDTH, FB_CELL_HEIGHT, paper);

    if (cell->character != ' ') {
        glyph_draw(x, y, cell->character, ink);
    }
}

/*
 * Does a repaint of the whole picture have to write this cell? A space in the
 * pair of the console is the paper the picture was cleared with, so it is
 * already right; a letter, the cursor or a colour of a command is not. The
 * check is what keeps a repaint to the cells that carry something, which is
 * the difference between drawing a command's line and writing a million
 * pixels of the same paper again.
 */
static bool cell_needs_paint(const struct fb_cell *cell, bool cursor)
{
    if (cursor || cell->character != ' ') {
        return true;
    }

    return cell->attr != g_theme_attr;
}

static void fb_clear(k_u8_t attr)
{
    int row;
    int column;

    for (row = 0; row < FB_ROWS_MAX; row++) {
        for (column = 0; column < FB_COLUMNS_MAX; column++) {
            g_cells[row][column].character = ' ';
            g_cells[row][column].attr = attr;
        }
    }

    {
        k_u16_t ink;
        k_u16_t paper;

        attr_colors(attr, false, &ink, &paper);
        if (!g_console_taken) {
            render_clear(NULL, paper);
        }
    }

    g_cursor_row = -1;
    g_cursor_column = -1;
}

static void fb_put(int row, int column, char character, k_u8_t attr)
{
    if (row < 0 || row >= g_rows || column < 0 || column >= g_columns) {
        return;
    }

    g_cells[row][column].character = character;
    g_cells[row][column].attr = attr;

    /* The grid follows the console even while a program owns the glass: the
     * line is not lost, it is painted when the screen comes back. */
    if (!g_console_taken) {
        cell_paint(row, column, false);
    }
}

/*
 * A row of cells up: the picture moves by the step of a line - the cell and
 * the gap of /etc/font - so the columns stay where they are and only the
 * lines change. The memory of the picture is one block, so the pixels are one
 * move of the rows that stay; the grid is the same move of a row of cells,
 * with the attribute of every cell travelling with its letter, and the last
 * line of the new picture is the paper of `attr`.
 */
static void fb_scroll(k_u8_t attr)
{
    int line = g_pitch * g_stride; /* pixels of one row of cells */
    int last = (g_rows - 1) * g_pitch;
    k_u16_t ink;
    k_u16_t paper;

    if (!g_console_taken) {
        memmove((void *)(uintptr_t)g_fb, (const void *)(uintptr_t)(g_fb + line),
                (k_size_t)(g_height - g_pitch) * (k_size_t)g_stride * sizeof(k_u16_t));

        attr_colors(attr, false, &ink, &paper);

        if (last < g_height) {
            area_fill(0, last, g_width, g_height - last, paper);
        }
    }

    memmove(g_cells[0], g_cells[1], (k_size_t)(FB_ROWS_MAX - 1) * sizeof(g_cells[0]));

    {
        int column;

        for (column = 0; column < FB_COLUMNS_MAX; column++) {
            g_cells[g_rows - 1][column].character = ' ';
            g_cells[g_rows - 1][column].attr = attr;
        }
    }

    /*
     * The cursor is the one cell the memory does not show as the grid says:
     * the block moved up with the picture, and the cell it left has to read
     * as its character again. The console names the new position after the
     * scroll; the old one is this file's to put right.
     */
    if (g_cursor_row > 0) {
        if (!g_console_taken) {
            cell_paint(g_cursor_row - 1, g_cursor_column, false);
        }
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
    if (g_cursor_row >= 0 && !g_console_taken) {
        cell_paint(g_cursor_row, g_cursor_column, false);
    }

    g_cursor_row = row;
    g_cursor_column = column;

    if (!g_console_taken) {
        cell_paint(row, column, true);
    }
}

/* The pair of `console`: the plain text repaints in the new colours and a
 * cell with a colour of its own - a warning, a directory - keeps it. */
static void fb_colors(const struct port_x86_64_pair *pair)
{
    k_u8_t old = g_theme_attr;
    k_u8_t next = PORT_X86_64_ATTR(pair->fg, pair->bg);
    int row;
    int column;

    g_ink = pair->ink;
    g_paper = pair->paper;
    g_theme_attr = next;

    for (row = 0; row < g_rows; row++) {
        for (column = 0; column < g_columns; column++) {
            struct fb_cell *cell = &g_cells[row][column];

            if (cell->attr == old) {
                cell->attr = next;
            } else if (cell->attr == attr_reverse(old)) {
                cell->attr = attr_reverse(next);
            }
        }
    }

    if (g_console_taken) {
        return;
    }

    render_clear(NULL, g_paper);

    for (row = 0; row < g_rows; row++) {
        for (column = 0; column < g_columns; column++) {
            bool cursor = row == g_cursor_row && column == g_cursor_column;

            if (cell_needs_paint(&g_cells[row][column], cursor)) {
                cell_paint(row, column, cursor);
            }
        }
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

    /* While a program owns the glass the kernel has no business painting it:
     * the grid is kept and FBIO_GIVE draws it. */
    if (g_fb == NULL || g_console_taken) {
        return;
    }

    render_clear(NULL, g_paper);

    for (row = 0; row < g_rows; row++) {
        for (column = 0; column < g_columns; column++) {
            bool cursor = row == g_cursor_row && column == g_cursor_column;

            if (cell_needs_paint(&g_cells[row][column], cursor)) {
                cell_paint(row, column, cursor);
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

/*
 * How many rows of cells a picture of `height` lines holds at the step of
 * `pitch`: the last one has to be whole, so the count stops at the last cell
 * whose sixteen lines fit - 800 lines are 50 rows of 16, 44 of 18 and 40 of
 * 20, and the two or six lines left over stay paper under the last row.
 */
static int rows_for(int height, int pitch)
{
    if (height < FONT8_HEIGHT) {
        return 0;
    }

    return (height - FONT8_HEIGHT) / pitch + 1;
}

/* The name of the screen: the mode and the grid the console counts in. */
static void screen_name(unsigned bpp)
{
    snprintf(g_name, sizeof(g_name), "the framebuffer of %ux%u, %u bpp, %dx%d cells", g_width,
             g_height, bpp, g_columns, g_rows);
}

/*
 * The gap between two lines, in pixels (the `line_gap` of /etc/font, ADR
 * 0021). The step of a line becomes 16 plus the gap, the rows of the grid are
 * counted again, and the picture is drawn from the cells the console holds -
 * the log is not lost, it is laid out again. A screen that is the text mode
 * of the VGA is not this file's: the caller asks first.
 */
void port_x86_64_fb_set_line_gap(int gap)
{
    int rows;
    int old_rows;

    if (g_fb == NULL) {
        return;
    }

    if (gap < 0 || gap > FB_LINE_GAP_MAX) {
        gap = 0;
    }

    rows = rows_for(g_height, FB_CELL_HEIGHT + gap);

    if (rows > FB_ROWS_MAX) {
        rows = FB_ROWS_MAX;
    }
    if (rows < 1) {
        rows = 1;
    }

    old_rows = g_rows;
    g_pitch = FB_CELL_HEIGHT + gap;
    g_rows = rows;
    s_screen.rows = rows;

    /*
     * The rows a wider gap takes away are gone: their cells are blanked so
     * that a gap that goes back to the whole cell does not bring back a line
     * of the log the console had already lost.
     */
    if (rows < old_rows) {
        int row;
        int column;

        for (row = rows; row < old_rows; row++) {
            for (column = 0; column < FB_COLUMNS_MAX; column++) {
                g_cells[row][column].character = ' ';
                g_cells[row][column].attr = g_theme_attr;
            }
        }
    }

    screen_name(PORT_X86_64_SCREEN_BPP);

    fb_redraw();
}

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

    g_pitch = FB_CELL_HEIGHT;
    g_columns = g_width / FB_CELL_WIDTH;
    g_rows = rows_for(g_height, g_pitch);

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
    g_paper = FB_BLACK;
    g_theme_attr = FB_ATTR_BOOT;

    screen_name(mode->bpp);

    s_screen.columns = g_columns;
    s_screen.rows = g_rows;

    /* The picture of the machine is the one the console of the kernel draws
     * on when a command asks for it (kernel/render.h). */
    k_render_register(&s_renderer);

    fb_clear(g_theme_attr);

    return &s_screen;
}

/* --- the device of ADR 0028 ------------------------------------------------- */

/*
 * The shape of the picture for `/dev/fb0`: false when the machine has no
 * framebuffer (the text mode of the VGA is the console of this port, not a
 * picture a program may draw in).
 */
bool port_x86_64_fb_info(struct fb_info *out)
{
    if (out == NULL || g_fb == NULL) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->width = (k_u16_t)g_width;
    out->height = (k_u16_t)g_height;
    out->pitch = (k_u16_t)g_stride;
    out->columns = (k_u16_t)g_columns;
    out->rows = (k_u16_t)g_rows;
    out->bpp = (k_u8_t)PORT_X86_64_SCREEN_BPP;
    out->format = FB_FORMAT_RGB565;
    out->font_width = FB_CELL_WIDTH;
    out->font_height = FB_CELL_HEIGHT;

    memcpy(out->palette, port_x86_64_palette, sizeof(out->palette));

    return true;
}

/*
 * One rectangle of a program's picture into the memory of the card. The
 * caller has checked the bounds (kernel/fb.c, fb_rect_parse); the check here
 * is the second door for a direct caller and costs one comparison a call.
 */
int port_x86_64_fb_blit(const struct fb_rect *rect, const k_u16_t *pixels)
{
    int row;

    if (rect == NULL || pixels == NULL || g_fb == NULL) {
        return KERR_INVAL;
    }

    if (rect->w == 0u || rect->h == 0u || (k_u32_t)rect->x + (k_u32_t)rect->w > (k_u32_t)g_width ||
        (k_u32_t)rect->y + (k_u32_t)rect->h > (k_u32_t)g_height) {
        return KERR_INVAL;
    }

    for (row = 0; row < (int)rect->h; row++) {
        k_u16_t *destination =
            (k_u16_t *)(uintptr_t)(g_fb + (k_size_t)(rect->y + row) * (k_size_t)g_stride +
                                   (k_size_t)rect->x);

        memcpy(destination, pixels + (k_size_t)row * (k_size_t)rect->w,
               (k_size_t)rect->w * sizeof(k_u16_t));
    }

    return 0;
}

/*
 * The screen changes hands. Taking it stops the console painting and keeps
 * its grid; giving it back paints the grid again, so the line the shell
 * printed while the program held the picture is there. Neither call saves or
 * restores a pixel of the program: the interface repaints what it needs.
 */
void port_x86_64_fb_console_take(void)
{
    if (g_fb != NULL) {
        g_console_taken = true;
    }
}

void port_x86_64_fb_console_give(void)
{
    if (!g_console_taken) {
        return;
    }

    g_console_taken = false;
    fb_redraw();
}

/*
 * The panic: the screen comes back whatever a program holds and the grid is
 * painted, because a machine that dies with an interface over the message is
 * a machine that says nothing (ADR 0028 §2).
 */
void port_x86_64_fb_console_force(void)
{
    g_console_taken = false;
    fb_redraw();
}

/* --- the font system of /etc (ADR 0021) ----------------------------------- */

/*
 * `/etc/font` for the screen of a PC: the gap between two lines. The scale,
 * the columns and the rows are the cap of the dialogue on the panel of the
 * board and mean nothing here - the font of this screen is the size it is,
 * and the grid of the picture is the whole picture - so a file that asks for
 * them is told once that the value is kept.
 *
 * A value that is not 0, 2 or 4 pixels is not a gap: the default of 0 is used
 * and the file is named, because a typo in /etc must not move a line of the
 * shell by a number nobody chose.
 */
static void apply_font(const char *system, const struct settings_table *table)
{
    const char *value = settings_value(table, "line_gap", "0");
    int gap;

    K_UNUSED(system);

    if (strcmp(value, "0") == 0) {
        gap = 0;
    } else if (strcmp(value, "2") == 0) {
        gap = 2;
    } else if (strcmp(value, "4") == 0) {
        gap = 4;
    } else {
        kwarn("font: line_gap '%s' is not 0, 2 or 4; the default 0 is used", value);
        gap = 0;
    }

    if (settings_number(table, "scale", 1, 1, 2) != 1 ||
        settings_number(table, "columns", 0, 0, 99) != 0 ||
        settings_number(table, "rows", 0, 0, 99) != 0) {
        kwarn("font: scale, columns and rows are the cap of the panel; the values are kept");
    }

    if (g_fb == NULL) {
        kwarn("font: line_gap %d is kept, the text mode of the VGA has no gap", gap);
        return;
    }

    port_x86_64_fb_set_line_gap(gap);

    kinfo("font: line_gap %d, the cell is %dx%d at %d px, %d rows of %d columns", gap, FONT8_WIDTH,
          FONT8_HEIGHT, FONT8_HEIGHT + gap, g_rows, g_columns);
}

static const struct settings_system s_font_system = {
    .name = "font",
    .apply = apply_font,
};

int port_x86_64_font_settings_register(void)
{
    return settings_register(&s_font_system);
}
