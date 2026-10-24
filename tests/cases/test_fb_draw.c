/*
 * Micro OS — the turn of the picture and the graphics.
 *
 * The mapping is checked against corners computed by hand for every
 * turn, and then as a whole: a turn has to be a one to one
 * correspondence between the picture and the panel — no pixel of the
 * panel left out, none used twice. The drawings are checked for what
 * they leave behind: a rectangle cut by the edge of the picture writes
 * only the part that is on it, a line touches its two ends, and the
 * same scene built from the numbers of the corpus drawn twice is the
 * same picture, byte for byte.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <stdlib.h>

#include "drivers/fb_draw.h"

#ifndef KTEST_CORPUS
#define KTEST_CORPUS "tests/corpus/vectors.txt"
#endif

/* The panel of the board in small numbers: a portrait 4x6. */
#define PW 4
#define PH 6
#define PIXELS (PW * PH)

static uint16_t g_buffer[PIXELS];

static void fill_buffer(uint16_t color)
{
    int i;

    for (i = 0; i < PIXELS; i++) {
        g_buffer[i] = color;
    }
}

static int count_color(uint16_t color)
{
    int count = 0;
    int i;

    for (i = 0; i < PIXELS; i++) {
        if (g_buffer[i] == color) {
            count++;
        }
    }

    return count;
}

/* The picture turns, and its sides swap on the quarter turns. */
static void test_sizes(void)
{
    struct fb_view view;

    ktest_begin("fb: the picture turns, its sides swap");

    fb_view_init(&view, g_buffer, PW, PH, 0);
    KT_CHECK_INT_EQ(fb_view_width(&view), 4);
    KT_CHECK_INT_EQ(fb_view_height(&view), 6);

    fb_view_init(&view, g_buffer, PW, PH, 1);
    KT_CHECK_INT_EQ(fb_view_width(&view), 6);
    KT_CHECK_INT_EQ(fb_view_height(&view), 4);

    fb_view_init(&view, g_buffer, PW, PH, 2);
    KT_CHECK_INT_EQ(fb_view_width(&view), 4);
    KT_CHECK_INT_EQ(fb_view_height(&view), 6);

    fb_view_init(&view, g_buffer, PW, PH, 3);
    KT_CHECK_INT_EQ(fb_view_width(&view), 6);
    KT_CHECK_INT_EQ(fb_view_height(&view), 4);

    /* anything else is taken modulo 4 */
    fb_view_init(&view, g_buffer, PW, PH, 5);
    KT_CHECK_INT_EQ(fb_view_width(&view), 6);

    /* a view that cannot be made is a view of nothing */
    fb_view_init(NULL, g_buffer, PW, PH, 0);
    KT_CHECK_INT_EQ(fb_view_width(NULL), 0);
    KT_CHECK_INT_EQ(fb_view_height(NULL), 0);
    KT_CHECK(!fb_view_map(NULL, 0, 0, NULL, NULL));
}

/*
 * The corners, by hand: turn the picture and the corner of the old
 * bottom left is the corner of the new top left (a quarter turn
 * clockwise), and so on around the square.
 */
static void test_corners(void)
{
    struct fb_view view;
    int px = -1;
    int py = -1;

    ktest_begin("fb: the corners land where a hand-computed turn puts them");

    /* 0: the portrait, nothing is turned */
    fb_view_init(&view, g_buffer, PW, PH, 0);
    KT_CHECK(fb_view_map(&view, 0, 0, &px, &py));
    KT_CHECK_INT_EQ(px, 0);
    KT_CHECK_INT_EQ(py, 0);
    KT_CHECK(fb_view_map(&view, 3, 5, &px, &py));
    KT_CHECK_INT_EQ(px, 3);
    KT_CHECK_INT_EQ(py, 5);

    /* 1: a quarter turn clockwise */
    fb_view_init(&view, g_buffer, PW, PH, 1);
    KT_CHECK(fb_view_map(&view, 0, 0, &px, &py));
    KT_CHECK_INT_EQ(px, 0);
    KT_CHECK_INT_EQ(py, 5);
    KT_CHECK(fb_view_map(&view, 5, 0, &px, &py));
    KT_CHECK_INT_EQ(px, 0);
    KT_CHECK_INT_EQ(py, 0);
    KT_CHECK(fb_view_map(&view, 0, 3, &px, &py));
    KT_CHECK_INT_EQ(px, 3);
    KT_CHECK_INT_EQ(py, 5);
    KT_CHECK(fb_view_map(&view, 5, 3, &px, &py));
    KT_CHECK_INT_EQ(px, 3);
    KT_CHECK_INT_EQ(py, 0);

    /* 2: half a turn */
    fb_view_init(&view, g_buffer, PW, PH, 2);
    KT_CHECK(fb_view_map(&view, 0, 0, &px, &py));
    KT_CHECK_INT_EQ(px, 3);
    KT_CHECK_INT_EQ(py, 5);
    KT_CHECK(fb_view_map(&view, 3, 5, &px, &py));
    KT_CHECK_INT_EQ(px, 0);
    KT_CHECK_INT_EQ(py, 0);

    /* 3: a quarter turn counter clockwise */
    fb_view_init(&view, g_buffer, PW, PH, 3);
    KT_CHECK(fb_view_map(&view, 0, 0, &px, &py));
    KT_CHECK_INT_EQ(px, 3);
    KT_CHECK_INT_EQ(py, 0);
    KT_CHECK(fb_view_map(&view, 5, 0, &px, &py));
    KT_CHECK_INT_EQ(px, 3);
    KT_CHECK_INT_EQ(py, 5);
    KT_CHECK(fb_view_map(&view, 0, 3, &px, &py));
    KT_CHECK_INT_EQ(px, 0);
    KT_CHECK_INT_EQ(py, 0);
    KT_CHECK(fb_view_map(&view, 5, 3, &px, &py));
    KT_CHECK_INT_EQ(px, 0);
    KT_CHECK_INT_EQ(py, 5);
}

/* A point outside the picture is not drawn, and nothing is written. */
static void test_outside(void)
{
    struct fb_view view;
    int px = 0;
    int py = 0;

    ktest_begin("fb: a point outside the picture is not drawn");

    fb_view_init(&view, g_buffer, PW, PH, 0);
    KT_CHECK(!fb_view_map(&view, -1, 0, &px, &py));
    KT_CHECK(!fb_view_map(&view, 0, -1, &px, &py));
    KT_CHECK(!fb_view_map(&view, PW, 0, &px, &py));
    KT_CHECK(!fb_view_map(&view, 0, PH, &px, &py));
    KT_CHECK(!fb_view_map(&view, 0, 0, NULL, &py));
    KT_CHECK(!fb_view_map(NULL, 0, 0, &px, &py));

    /* the landscape has other edges */
    fb_view_init(&view, g_buffer, PW, PH, 1);
    KT_CHECK(!fb_view_map(&view, 6, 0, &px, &py));
    KT_CHECK(!fb_view_map(&view, 0, 4, &px, &py));
    KT_CHECK(fb_view_map(&view, 5, 3, &px, &py));

    /* the drawings of outside points leave the panel alone */
    fill_buffer(0x1234);
    fb_view_pixel(&view, 6, 0, 0xFFFF);
    fb_view_pixel(&view, -1, -1, 0xFFFF);
    fb_view_fill_rect(&view, 4, 4, 10, 10, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0x1234), PIXELS);
}

/*
 * Every turn is a one to one correspondence: over the whole picture
 * each pixel of the panel is met exactly once — none is left out of
 * the picture, none is used twice (two picture points fighting over
 * one panel pixel would be an image with a hole and a pile-up).
 */
static void test_bijection(void)
{
    int rotation;

    ktest_begin("fb: every turn uses every pixel of the panel once");

    for (rotation = 0; rotation < 4; rotation++) {
        struct fb_view view;
        int seen[PIXELS];
        int x;
        int y;
        int i;

        fb_view_init(&view, g_buffer, PW, PH, rotation);

        for (i = 0; i < PIXELS; i++) {
            seen[i] = 0;
        }

        for (y = 0; y < fb_view_height(&view); y++) {
            for (x = 0; x < fb_view_width(&view); x++) {
                int px = -1;
                int py = -1;

                if (!fb_view_map(&view, x, y, &px, &py) || px < 0 || px >= PW || py < 0 ||
                    py >= PH) {
                    ktest_fail(__FILE__, __LINE__, "rotation %d: (%d,%d) is off the panel",
                               rotation, x, y);
                    return;
                }

                seen[py * PW + px]++;
            }
        }

        for (i = 0; i < PIXELS; i++) {
            if (seen[i] != 1) {
                ktest_fail(__FILE__, __LINE__, "rotation %d: panel pixel %d used %d times",
                           rotation, i, seen[i]);
                return;
            }
        }
    }
}

/* A pixel of the picture lands on the right pixel of the panel. */
static void test_pixel(void)
{
    struct fb_view view;

    ktest_begin("fb: a pixel lands on the panel pixel its turn says");

    fill_buffer(0x0000);
    fb_view_init(&view, g_buffer, PW, PH, 1);
    fb_view_pixel(&view, 5, 0, 0xFFFF); /* the top right of the landscape */
    KT_CHECK_INT_EQ(count_color(0xFFFF), 1);
    KT_CHECK_INT_EQ(g_buffer[0], 0xFFFF); /* the top left of the portrait */

    fill_buffer(0x0000);
    fb_view_init(&view, g_buffer, PW, PH, 3);
    fb_view_pixel(&view, 0, 0, 0xFFFF); /* the top right of the portrait */
    KT_CHECK_INT_EQ(count_color(0xFFFF), 1);
    KT_CHECK_INT_EQ(g_buffer[3], 0xFFFF);
}

/* A rectangle fills itself, and the edge of the picture cuts it. */
static void test_fill_rect(void)
{
    struct fb_view view;

    ktest_begin("fb: a rectangle fills itself and is clipped by the edge");

    fb_view_init(&view, g_buffer, PW, PH, 0);

    fill_buffer(0x0000);
    fb_view_fill_rect(&view, 1, 1, 2, 2, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 4);
    KT_CHECK_INT_EQ(g_buffer[1 * PW + 1], 0xFFFF);
    KT_CHECK_INT_EQ(g_buffer[2 * PW + 2], 0xFFFF);
    KT_CHECK_INT_EQ(g_buffer[0], 0x0000);
    KT_CHECK_INT_EQ(g_buffer[3 * PW + 3], 0x0000);

    /* cut by the corner: only the part that is on the picture */
    fill_buffer(0x0000);
    fb_view_fill_rect(&view, -1, -1, 3, 3, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 4);
    KT_CHECK_INT_EQ(g_buffer[0], 0xFFFF);
    KT_CHECK_INT_EQ(g_buffer[1 * PW + 1], 0xFFFF);
    KT_CHECK_INT_EQ(g_buffer[2 * PW + 2], 0x0000);

    /* one pixel of a rectangle past the edge is one pixel */
    fill_buffer(0x0000);
    fb_view_fill_rect(&view, 3, 5, 10, 10, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 1);
    KT_CHECK_INT_EQ(g_buffer[5 * PW + 3], 0xFFFF);

    /* a rectangle with no size fills nothing */
    fill_buffer(0x0000);
    fb_view_fill_rect(&view, 0, 0, 0, 10, 0xFFFF);
    fb_view_fill_rect(&view, 0, 0, 10, 0, 0xFFFF);
    fb_view_fill_rect(&view, 0, 0, -5, -5, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 0);

    /* in the landscape the fill goes through the turn */
    fill_buffer(0x0000);
    fb_view_init(&view, g_buffer, PW, PH, 1);
    fb_view_fill_rect(&view, 4, 1, 2, 2, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 4);
    KT_CHECK_INT_EQ(g_buffer[1 * PW + 1], 0xFFFF); /* (4,1) of the picture */
}

/* The outline is one pixel thick and the middle stays empty. */
static void test_rect(void)
{
    struct fb_view view;

    ktest_begin("fb: a rectangle draws only its border");

    fb_view_init(&view, g_buffer, PW, PH, 0);

    fill_buffer(0x0000);
    fb_view_rect(&view, 0, 0, 3, 3, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 8);
    KT_CHECK_INT_EQ(g_buffer[1 * PW + 1], 0x0000); /* the middle of the 3x3 */

    /* cut by the corner: the visible part of the border, and nothing else */
    fill_buffer(0x0000);
    fb_view_rect(&view, -1, -1, 3, 3, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 3);
    KT_CHECK_INT_EQ(g_buffer[0], 0x0000); /* still the middle of that rectangle */
}

/* A line fills a column, a row, and a diagonal from end to end. */
static void test_line(void)
{
    struct fb_view view;
    int i;

    ktest_begin("fb: a line is the pixels between its two ends");

    fb_view_init(&view, g_buffer, PW, PH, 0);

    fill_buffer(0x0000);
    fb_view_line(&view, 2, 0, 2, 5, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 6);
    for (i = 0; i < 6; i++) {
        KT_CHECK_INT_EQ(g_buffer[i * PW + 2], 0xFFFF);
    }

    fill_buffer(0x0000);
    fb_view_line(&view, 0, 3, 3, 3, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 4);

    /* the diagonal of a 4x6: six pixels, both ends on it */
    fill_buffer(0x0000);
    fb_view_line(&view, 0, 0, 3, 5, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 6);
    KT_CHECK_INT_EQ(g_buffer[0], 0xFFFF);          /* the start */
    KT_CHECK_INT_EQ(g_buffer[5 * PW + 3], 0xFFFF); /* the end */

    /* a line whose ends are outside still draws the part on the picture */
    fill_buffer(0x0000);
    fb_view_line(&view, -5, -5, 10, 10, 0xFFFF);
    KT_CHECK_INT_EQ(count_color(0xFFFF), 4); /* the diagonal pixels of the portrait */
}

/*
 * The corpus: the real numbers make the scene (its place, its size,
 * its colour, its turn), and the scene drawn twice is the same picture
 * — every time, on every turn.
 */
static void test_corpus(void)
{
    FILE *file = fopen(KTEST_CORPUS, "r");
    char line[128];
    int checked = 0;

    ktest_begin("fb: the numbers of the corpus, twice the same picture");

    KT_CHECK(file != NULL);
    if (file == NULL) {
        return;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        double value = strtod(line, NULL);
        long long magnitude = (value < 0.0) ? (long long)(-value) : (long long)value;
        int rotation = (int)(magnitude % 4);
        int x = (int)(magnitude % 7) - 3; /* -3..3: partly off the picture */
        int y = (int)((magnitude / 7) % 7) - 3;
        int w = (int)((magnitude / 49) % 9); /* 0..8 */
        int h = (int)((magnitude / 441) % 9);
        uint16_t color = (uint16_t)(magnitude & 0xFFFFu);
        struct fb_view view;
        uint16_t first[PIXELS];

        fb_view_init(&view, g_buffer, PW, PH, rotation);

        fill_buffer(0x0000);
        fb_view_fill_rect(&view, x, y, w, h, color);
        fb_view_line(&view, x, y, x + w, y + h, color);
        memcpy(first, g_buffer, sizeof(first));

        fill_buffer(0x0000);
        fb_view_fill_rect(&view, x, y, w, h, color);
        fb_view_line(&view, x, y, x + w, y + h, color);

        ktest_checks++;
        if (memcmp(first, g_buffer, sizeof(first)) != 0) {
            ktest_fail(__FILE__, __LINE__, "the scene of %lld drawn twice differs", magnitude);
            break;
        }

        checked++;
    }

    fclose(file);

    ktest_checks++;
    if (checked == 0) {
        ktest_fail(__FILE__, __LINE__, "no scenes from the corpus");
    }
}

void test_fb_draw(void)
{
    test_sizes();
    test_corners();
    test_outside();
    test_bijection();
    test_pixel();
    test_fill_rect();
    test_rect();
    test_line();
    test_corpus();
}
