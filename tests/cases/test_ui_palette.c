/*
 * Micro OS — the palette of the desktop (ui/palette.h).
 *
 * The console keeps the sixteen colours of the CGA, because a terminal has
 * sixteen attributes; the desktop does not, and this test holds the roles of
 * its two themes without a screen: eight bits a channel narrowed to the
 * five-six-five of ADR 0023, a gradient of the wallpaper, the blend of the
 * shadow.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stddef.h>

#include "ui/palette.h"

/* The brightness of a colour of the screen: the three channels widened and
 * weighted the way a person sees them. */
static int brightness(k_u16_t color)
{
    int red = ((color >> 11) & 0x1F) * 255 / 31;
    int green = ((color >> 5) & 0x3F) * 255 / 63;
    int blue = (color & 0x1F) * 255 / 31;

    return (2126 * red + 7152 * green + 722 * blue) / 10000;
}

static void test_ui_palette_channels(void)
{
    ktest_begin("ui: a colour of eight bits a channel becomes five-six-five");

    KT_CHECK_INT_EQ(UI_RGB(0, 0, 0), 0x0000u);
    KT_CHECK_INT_EQ(UI_RGB(255, 255, 255), 0xFFFFu);
    KT_CHECK_INT_EQ(UI_RGB(255, 0, 0), 0xF800u);
    KT_CHECK_INT_EQ(UI_RGB(0, 255, 0), 0x07E0u);
    KT_CHECK_INT_EQ(UI_RGB(0, 0, 255), 0x001Fu);

    /* The low bits of a channel are dropped and not rounded: 0x07 is no red
     * at all, 0x08 is the first step of the five the screen has. */
    KT_CHECK_INT_EQ(UI_RGB(7, 3, 7), 0x0000u);
    KT_CHECK_INT_EQ(UI_RGB(8, 4, 8), 0x0821u);
}

static void test_ui_palette_themes(void)
{
    struct ui_palette dark;
    struct ui_palette light;
    struct ui_palette named;

    ktest_begin("ui: the dark and the light theme are two readings of one desk");

    ui_palette_dark(&dark);
    ui_palette_light(&light);

    /* The ink of a theme is read on the surface of a window: the dark theme is
     * light ink on a dark surface, the light theme the other way round. */
    KT_CHECK(brightness(dark.ink) > brightness(dark.surface));
    KT_CHECK(brightness(light.ink) < brightness(light.surface));

    /* The hierarchy of one font: the title is brighter than the body and the
     * body brighter than the line that says the second thing. */
    KT_CHECK(brightness(dark.title_ink) > brightness(dark.ink));
    KT_CHECK(brightness(dark.ink) > brightness(dark.ink_dim));
    KT_CHECK(brightness(light.title_ink) < brightness(light.ink));
    KT_CHECK(brightness(light.ink) < brightness(light.ink_dim));

    /* The roles that have to be seen at all are not the surface they sit on. */
    KT_CHECK(dark.accent != dark.surface && dark.accent != dark.background_top);
    KT_CHECK(dark.border != dark.surface);
    KT_CHECK(light.accent != light.surface && light.accent != light.background_top);

    /* The two themes do not share one role: a light desk is not a dark one
     * with a word changed. */
    KT_CHECK(dark.surface != light.surface);
    KT_CHECK(dark.ink != light.ink);
    KT_CHECK(dark.background_top != light.background_top);
    KT_CHECK(dark.accent != light.accent);

    /* The name in /etc/console picks the palette; an unknown word is the
     * default of the file and not an error of the desktop. */
    ui_palette_named(&named, "light");
    KT_CHECK(named.surface == light.surface);
    ui_palette_named(&named, "dark");
    KT_CHECK(named.surface == dark.surface);
    ui_palette_named(&named, "sunset");
    KT_CHECK(named.surface == dark.surface);
    ui_palette_named(&named, NULL);
    KT_CHECK(named.surface == dark.surface);
}

static void test_ui_palette_gradient(void)
{
    struct ui_palette dark;
    k_u16_t top;
    k_u16_t middle;
    k_u16_t bottom;

    ktest_begin("ui: the wallpaper is a gradient and not two bands");

    ui_palette_dark(&dark);

    top = ui_palette_gradient(&dark, 0, 800);
    middle = ui_palette_gradient(&dark, 400, 800);
    bottom = ui_palette_gradient(&dark, 799, 800);

    KT_CHECK_INT_EQ(top, dark.background_top);
    KT_CHECK_INT_EQ(bottom, dark.background_bottom);
    KT_CHECK(top != middle && middle != bottom);
    KT_CHECK(brightness(middle) > brightness(top));
    KT_CHECK(brightness(middle) < brightness(bottom));

    /* The row is the row of the picture and not of the rectangle: the same y
     * of two heights is two colours only because the pictures differ. */
    KT_CHECK_INT_EQ(ui_palette_gradient(&dark, 0, 1), dark.background_top);
    KT_CHECK_INT_EQ(ui_palette_gradient(&dark, 7, 1), dark.background_top);
    KT_CHECK_INT_EQ(ui_palette_gradient(NULL, 1, 800), 0x0000u);
}

static void test_ui_palette_blend(void)
{
    struct ui_palette dark;
    k_u16_t black = 0x0000u;
    k_u16_t white = 0xFFFFu;
    k_u16_t half;
    k_u16_t shade;
    k_u16_t shadow;

    ktest_begin("ui: the shadow of a window is a blend of the wallpaper");

    half = ui_palette_blend(black, white, 128);
    KT_CHECK_INT_EQ(ui_palette_blend(black, white, 0), black);
    KT_CHECK_INT_EQ(ui_palette_blend(black, white, 255), white);
    KT_CHECK(brightness(half) > 100 && brightness(half) < 155);

    /* The shadow is darker than the desk it falls on, and the shade of a
     * colour is the same arithmetic. */
    ui_palette_dark(&dark);
    shadow = ui_palette_shadow(&dark, 400, 800);
    KT_CHECK(brightness(shadow) < brightness(ui_palette_gradient(&dark, 400, 800)));
    KT_CHECK(brightness(shadow) > 0);

    shade = ui_palette_shade(0xFFFFu, 50);
    KT_CHECK(brightness(shade) > 100 && brightness(shade) < 155);
    KT_CHECK_INT_EQ(ui_palette_shade(0xFFFFu, 0), 0x0000u);
    KT_CHECK_INT_EQ(ui_palette_shade(0xFFFFu, 100), 0xFFFFu);
}

void test_ui_palette(void)
{
    test_ui_palette_channels();
    test_ui_palette_themes();
    test_ui_palette_gradient();
    test_ui_palette_blend();
}
