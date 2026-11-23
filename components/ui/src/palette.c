/*
 * Micro OS — the colours of the desktop (ui/palette.h).
 *
 * The two themes are written the way a designer writes them, eight bits a
 * channel, and narrowed to the screen by `UI_RGB` once. The dark theme is a
 * slate desk with an azure accent; the light theme is the same desk under a
 * window at noon, and the roles keep their meaning, so a program that asks for
 * the ink gets a colour a person can read in either.
 */
#include "ui/palette.h"

#include <stddef.h>

#include "kernel/types.h"

void ui_palette_dark(struct ui_palette *palette)
{
    if (palette == NULL) {
        return;
    }

    palette->background_top = UI_RGB(16, 19, 27);
    palette->background_bottom = UI_RGB(28, 37, 54);
    palette->grid = UI_RGB(20, 26, 36);
    palette->surface = UI_RGB(24, 29, 39);
    palette->surface_high = UI_RGB(36, 44, 59);
    palette->panel = UI_RGB(20, 25, 35);
    palette->ink = UI_RGB(216, 223, 235);
    palette->ink_dim = UI_RGB(146, 160, 181);
    palette->title_ink = UI_RGB(244, 248, 255);
    palette->accent = UI_RGB(76, 141, 255);
    palette->accent_ink = UI_RGB(255, 255, 255);
    palette->border = UI_RGB(58, 70, 92);
    palette->selection = UI_RGB(46, 63, 92);
    palette->selection_ink = UI_RGB(255, 255, 255);
    palette->close = UI_RGB(208, 80, 75);
    palette->close_ink = UI_RGB(255, 255, 255);
    palette->shadow = UI_RGB(0, 0, 0);
}

void ui_palette_light(struct ui_palette *palette)
{
    if (palette == NULL) {
        return;
    }

    palette->background_top = UI_RGB(238, 242, 248);
    palette->background_bottom = UI_RGB(203, 213, 228);
    palette->grid = UI_RGB(216, 226, 237);
    palette->surface = UI_RGB(255, 255, 255);
    palette->surface_high = UI_RGB(227, 233, 242);
    palette->panel = UI_RGB(220, 227, 238);
    palette->ink = UI_RGB(29, 36, 48);
    palette->ink_dim = UI_RGB(93, 106, 126);
    palette->title_ink = UI_RGB(14, 19, 28);
    palette->accent = UI_RGB(31, 111, 235);
    palette->accent_ink = UI_RGB(255, 255, 255);
    palette->border = UI_RGB(169, 182, 201);
    palette->selection = UI_RGB(207, 224, 255);
    palette->selection_ink = UI_RGB(14, 19, 28);
    palette->close = UI_RGB(192, 66, 61);
    palette->close_ink = UI_RGB(255, 255, 255);
    palette->shadow = UI_RGB(0, 0, 0);
}

void ui_palette_named(struct ui_palette *palette, const char *name)
{
    if (name != NULL && name[0] == 'l' && name[1] == 'i' && name[2] == 'g' && name[3] == 'h' &&
        name[4] == 't' && name[5] == '\0') {
        ui_palette_light(palette);
        return;
    }

    ui_palette_dark(palette);
}

static unsigned channel_mix(unsigned below, unsigned above, unsigned alpha)
{
    return (below * (255u - alpha) + above * alpha + 127u) / 255u;
}

k_u16_t ui_palette_blend(k_u16_t below, k_u16_t above, unsigned alpha)
{
    unsigned red_below = (below >> 11) & 0x1Fu;
    unsigned green_below = (below >> 5) & 0x3Fu;
    unsigned blue_below = below & 0x1Fu;
    unsigned red_above = (above >> 11) & 0x1Fu;
    unsigned green_above = (above >> 5) & 0x3Fu;
    unsigned blue_above = above & 0x1Fu;

    if (alpha > 255u) {
        alpha = 255u;
    }

    return (k_u16_t)((channel_mix(red_below, red_above, alpha) << 11) |
                     (channel_mix(green_below, green_above, alpha) << 5) |
                     channel_mix(blue_below, blue_above, alpha));
}

k_u16_t ui_palette_shade(k_u16_t color, unsigned percent)
{
    if (percent > 100u) {
        percent = 100u;
    }

    return ui_palette_blend(0x0000u, color, percent * 255u / 100u);
}

k_u16_t ui_palette_gradient(const struct ui_palette *palette, int y, int height)
{
    unsigned alpha;
    long span;

    if (palette == NULL || height <= 1) {
        return palette != NULL ? palette->background_top : 0x0000u;
    }

    if (y <= 0) {
        return palette->background_top;
    }
    if (y >= height - 1) {
        return palette->background_bottom;
    }

    /* The row is a fraction of the whole picture and not of the rectangle
     * being painted: a damage of fifteen rows is the colour of the picture at
     * those fifteen rows (docs/desktop.md). */
    span = (long)(height - 1);
    alpha = (unsigned)((long)y * 255 / span);

    return ui_palette_blend(palette->background_top, palette->background_bottom, alpha);
}

k_u16_t ui_palette_shadow(const struct ui_palette *palette, int y, int height)
{
    if (palette == NULL) {
        return 0x0000u;
    }

    return ui_palette_blend(ui_palette_gradient(palette, y, height), palette->shadow,
                            UI_PALETTE_SHADOW_ALPHA);
}
