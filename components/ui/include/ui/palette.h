/*
 * Micro OS — the palette of the desktop (docs/desktop.md).
 *
 * The console of the kernel is sixteen colours of the CGA, because it is the
 * screen of a VGA and a terminal has sixteen attributes by definition. The
 * desktop is not a terminal: its picture is a framebuffer of sixty-five
 * thousand colours, and a window that paints the black, the bright cyan and
 * the yellow of the palette of 1981 is a window that says "DOS" (the
 * cleanroom of 1.7 said exactly that).
 *
 * A palette is a list of *roles* and not of colours: a program asks for the
 * surface of a window or the ink of a title and does not know the number. The
 * roles are eight bits a channel - the language a person writes a colour in -
 * and `UI_RGB` narrows them to the five-six-five of the screen once, here.
 * The two palettes are the dark and the light theme; the `theme` of
 * `/etc/console` picks the one the desktop starts with, so the console and the
 * desktop of one machine have one mood.
 *
 * The palette draws nothing: `ui/draw.h` is the picture and this file is the
 * colours it uses. A test on the host holds the roles without a screen.
 */
#ifndef MICRO_OS_UI_PALETTE_H
#define MICRO_OS_UI_PALETTE_H

#include "kernel/types.h"

/*
 * A colour written the way a person writes one: eight bits a channel, packed
 * into the five-six-five of ADR 0023. The three bits of red and blue and the
 * two of green that do not fit are dropped, not rounded - the difference is
 * one step of the device and the palette is read by a person, not measured.
 */
#define UI_RGB(red, green, blue)                                                                   \
    ((k_u16_t)((((k_u16_t)(red) & 0xF8u) << 8) | (((k_u16_t)(green) & 0xFCu) << 3) |               \
               (((k_u16_t)(blue) & 0xF8u) >> 3)))

/* How much of the shadow colour a window casts on the wallpaper, of 255. */
#define UI_PALETTE_SHADOW_ALPHA 110u

/*
 * The roles of the desktop. The wallpaper is a gradient from the top of the
 * picture to the bottom and has a grid of one pixel every `UI_PALETTE_GRID`
 * pixels; a window is its body (`surface`) with a bar of its title and a line
 * of status (`surface_high`); the panel is its own surface, a little darker
 * than a window so that it reads as the edge of the desk.
 *
 * `ink` is the text of a body, `ink_dim` the text a person reads second (a
 * line of status, the label of a button without the keys) and `title_ink` the
 * one of a title: three weights with one font, which is the hierarchy of a
 * desktop of 8x16 cells. `accent` is the focus - the ring of a window, the
 * border of the one with the keys, the button of the panel that is up.
 */
struct ui_palette {
    k_u16_t background_top;
    k_u16_t background_bottom;
    k_u16_t grid;
    k_u16_t surface;
    k_u16_t surface_high;
    k_u16_t panel;
    k_u16_t ink;
    k_u16_t ink_dim;
    k_u16_t title_ink;
    k_u16_t accent;
    k_u16_t accent_ink;
    k_u16_t border;
    k_u16_t selection;
    k_u16_t selection_ink;
    k_u16_t close;
    k_u16_t close_ink;
    k_u16_t shadow;
};

/* The step of the grid of the wallpaper, in pixels, and the offset of the
 * first line: the lines are not on the top and the left edge of the picture. */
#define UI_PALETTE_GRID 32
#define UI_PALETTE_GRID_OFFSET 16

/* The two themes, a whole palette each. */
void ui_palette_dark(struct ui_palette *palette);
void ui_palette_light(struct ui_palette *palette);

/*
 * The palette of a name of the `theme` of /etc/console: "light" is the light
 * theme and anything else - the word "dark", an empty string, a value this
 * version does not know - is the dark one, which is the default of the file.
 */
void ui_palette_named(struct ui_palette *palette, const char *name);

/*
 * `alpha` of `above` over `below`, of 255: the shadow of a window and every
 * other transparency of the desktop. The three channels are mixed in their
 * own width and packed again.
 */
k_u16_t ui_palette_blend(k_u16_t below, k_u16_t above, unsigned alpha);

/* `percent` of a colour over black: a darker tone of it. */
k_u16_t ui_palette_shade(k_u16_t color, unsigned percent);

/*
 * The colour of the wallpaper at the row `y` of a picture of `height` rows:
 * the linear gradient of the theme, `background_top` at the top and
 * `background_bottom` at the last row.
 */
k_u16_t ui_palette_gradient(const struct ui_palette *palette, int y, int height);

/*
 * The shadow of a window over the wallpaper at the row `y`: the shadow role
 * of the palette blended over the gradient. It is a colour of the wallpaper
 * and not of the window under it, which is what a drop shadow of three pixels
 * looks like on a dark desk and on a light one alike.
 */
k_u16_t ui_palette_shadow(const struct ui_palette *palette, int y, int height);

#endif /* MICRO_OS_UI_PALETTE_H */
