/*
 * Micro OS — the turning of the picture and the graphics (the pure part).
 *
 * See drivers/fb_draw.h: the coordinates belong to the picture the
 * viewer sees, and the mapping puts them onto the panel in its own
 * portrait.
 */
#include "drivers/fb_draw.h"

void fb_view_init(struct fb_view *view, uint16_t *pixels, int width, int height, int rotation)
{
    if (view == NULL) {
        return;
    }

    view->pixels = pixels;
    view->width = width;
    view->height = height;
    view->rotation = rotation & 3;
}

int fb_view_width(const struct fb_view *view)
{
    if (view == NULL) {
        return 0;
    }

    return (view->rotation & 1) != 0 ? view->height : view->width;
}

int fb_view_height(const struct fb_view *view)
{
    if (view == NULL) {
        return 0;
    }

    return (view->rotation & 1) != 0 ? view->width : view->height;
}

bool fb_view_map(const struct fb_view *view, int x, int y, int *px, int *py)
{
    if (view == NULL || px == NULL || py == NULL) {
        return false;
    }

    if (x < 0 || y < 0 || x >= fb_view_width(view) || y >= fb_view_height(view)) {
        return false; /* the point is not on this picture */
    }

    switch (view->rotation & 3) {
    case 0: /* the portrait, nothing is turned */
        *px = x;
        *py = y;
        break;

    case 1: /* a quarter turn clockwise: the old bottom left is the new top left */
        *px = y;
        *py = view->height - 1 - x;
        break;

    case 2: /* half a turn: the old bottom right is the new top left */
        *px = view->width - 1 - x;
        *py = view->height - 1 - y;
        break;

    default: /* three quarter turns clockwise (one counter clockwise) */
        *px = view->width - 1 - y;
        *py = x;
        break;
    }

    return true;
}

void fb_view_pixel(struct fb_view *view, int x, int y, uint16_t color)
{
    int px = 0;
    int py = 0;

    if (fb_view_map(view, x, y, &px, &py)) {
        view->pixels[(k_size_t)py * (k_size_t)view->width + (k_size_t)px] = color;
    }
}

void fb_view_fill_rect(struct fb_view *view, int x, int y, int w, int h, uint16_t color)
{
    int x0;
    int y0;
    int x1;
    int y1;
    int row;

    if (view == NULL || w <= 0 || h <= 0) {
        return;
    }

    /* The part of the rectangle that is actually on the picture. */
    x0 = (x < 0) ? 0 : x;
    y0 = (y < 0) ? 0 : y;
    x1 = x + w;
    y1 = y + h;

    if (x1 > fb_view_width(view)) {
        x1 = fb_view_width(view);
    }
    if (y1 > fb_view_height(view)) {
        y1 = fb_view_height(view);
    }

    for (row = y0; row < y1; row++) {
        int column;

        for (column = x0; column < x1; column++) {
            fb_view_pixel(view, column, row, color);
        }
    }
}

void fb_view_rect(struct fb_view *view, int x, int y, int w, int h, uint16_t color)
{
    int i;

    if (view == NULL || w <= 0 || h <= 0) {
        return;
    }

    for (i = x; i < x + w; i++) {
        fb_view_pixel(view, i, y, color);
        fb_view_pixel(view, i, y + h - 1, color);
    }

    for (i = y; i < y + h; i++) {
        fb_view_pixel(view, x, i, color);
        fb_view_pixel(view, x + w - 1, i, color);
    }
}

void fb_view_line(struct fb_view *view, int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        int e2;

        fb_view_pixel(view, x0, y0, color);

        if (x0 == x1 && y0 == y1) {
            break;
        }

        e2 = 2 * err;

        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}
