/*
 * Micro OS — the model, the canvas and the keyboard of an interface (ADR 0030).
 *
 * The library of `components/ui` is the userland of the picture: a window is
 * a rectangle in an order, a canvas is the pixels of a rectangle, and the
 * keyboard is the layout of the machine as a program reads it. None of the
 * three needs a screen or a port, so the host build holds them here; the
 * programs of `/bin` (ui, edit, fm) are the callers that make them real.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stddef.h>
#include <string.h>

#include "kernel/input.h"
#include "ui/draw.h"
#include "ui/keyboard.h"
#include "ui/window.h"

/* The picture of the test: the screen of ADR 0023, seven cells of eight by
 * sixteen pixels in a window of forty-eight columns. */
#define TEST_SCREEN_W 384
#define TEST_SCREEN_H 200

static void test_ui_rects(void)
{
    struct ui_rect a;
    struct ui_rect b;
    struct ui_rect out;

    ktest_begin("ui: a rectangle knows its points and its parts");

    a.x = 10;
    a.y = 20;
    a.w = 30;
    a.h = 40;

    KT_CHECK(ui_rect_contains(&a, 10, 20));  /* the top left pixel */
    KT_CHECK(ui_rect_contains(&a, 39, 59));  /* the last pixel */
    KT_CHECK(!ui_rect_contains(&a, 40, 20)); /* the right edge is outside */
    KT_CHECK(!ui_rect_contains(&a, 10, 60)); /* the bottom edge is outside */
    KT_CHECK(!ui_rect_contains(&a, 9, 20));
    KT_CHECK(!ui_rect_contains(NULL, 10, 20));

    b.x = 20;
    b.y = 30;
    b.w = 30;
    b.h = 40;
    KT_CHECK(ui_rect_intersect(&a, &b, &out));
    KT_CHECK_INT_EQ(out.x, 20);
    KT_CHECK_INT_EQ(out.y, 30);
    KT_CHECK_INT_EQ(out.w, 20);
    KT_CHECK_INT_EQ(out.h, 30);

    /* Two rectangles that only touch at an edge share no pixel. */
    b.x = 40;
    b.y = 0;
    b.w = 10;
    b.h = 10;
    KT_CHECK(!ui_rect_intersect(&a, &b, &out));
    KT_CHECK(!ui_rect_intersect(NULL, &b, &out));
    KT_CHECK(!ui_rect_intersect(&a, &b, NULL));

    /* The bounds of a screen: the same rule as the device of the picture. */
    b.x = 0;
    b.y = 0;
    b.w = TEST_SCREEN_W;
    b.h = TEST_SCREEN_H;
    KT_CHECK(ui_rect_fits(&a, &b));
    a.x = TEST_SCREEN_W - 1;
    a.w = 2;
    KT_CHECK(!ui_rect_fits(&a, &b));
    a.x = 0;
    a.w = 0;
    KT_CHECK(!ui_rect_fits(&a, &b));
}

static void test_ui_screen(void)
{
    struct ui_screen screen;
    struct ui_rect bounds;
    struct ui_rect first;
    struct ui_rect second;
    struct ui_rect modal;
    struct ui_rect bad;

    ktest_begin("ui: the windows are an order and a focus, not a list of names");

    bounds.x = 0;
    bounds.y = 0;
    bounds.w = TEST_SCREEN_W;
    bounds.h = TEST_SCREEN_H;
    ui_screen_init(&screen, &bounds);

    KT_CHECK_INT_EQ((int)screen.count, 0);
    KT_CHECK_INT_EQ(screen.focus, UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(screen.modal, UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(ui_screen_input(&screen), UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(ui_screen_focus_next(&screen, 1), UI_WINDOW_NONE);

    first.x = 0;
    first.y = 0;
    first.w = 100;
    first.h = 50;
    second.x = 20;
    second.y = 20;
    second.w = 100;
    second.h = 50;
    modal.x = 40;
    modal.y = 40;
    modal.w = 100;
    modal.h = 50;

    /* The first window arrives on top with the focus. */
    KT_CHECK_INT_EQ(ui_screen_add(&screen, 1, "one", &first, 0), 0);
    KT_CHECK_INT_EQ(screen.focus, 0);
    KT_CHECK_INT_EQ(ui_screen_find(&screen, 1), 0);
    KT_CHECK_INT_EQ(ui_screen_find(&screen, 2), UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(ui_screen_input(&screen), 0);

    /* The second is above the first and has the keys. */
    KT_CHECK_INT_EQ(ui_screen_add(&screen, 2, "two", &second, 0), 1);
    KT_CHECK_INT_EQ(screen.focus, 1);
    KT_CHECK_INT_EQ(ui_screen_top_at(&screen, 25, 25), 1);
    KT_CHECK_INT_EQ(ui_screen_top_at(&screen, 5, 5), 0);
    KT_CHECK_INT_EQ(ui_screen_top_at(&screen, 200, 150), UI_WINDOW_NONE);

    /* A duplicate id, a rectangle outside and the seventeenth window are the
     * three refusals of the screen. */
    KT_CHECK_INT_EQ(ui_screen_add(&screen, 2, "again", &modal, 0), UI_WINDOW_NONE);
    bad.x = TEST_SCREEN_W - 1;
    bad.y = 0;
    bad.w = 10;
    bad.h = 10;
    KT_CHECK_INT_EQ(ui_screen_add(&screen, 3, "outside", &bad, 0), UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(ui_screen_add(&screen, 3, "", &modal, 0), UI_WINDOW_NONE);

    /* The focus goes round the order and wraps. */
    KT_CHECK_INT_EQ(ui_screen_focus_next(&screen, 1), 0);
    KT_CHECK_INT_EQ(ui_screen_focus_next(&screen, 1), 1);
    KT_CHECK_INT_EQ(ui_screen_focus_next(&screen, -1), 0);

    /* Raising the bottom window puts it on top and takes the keys; the index
     * of the other one moved down. */
    KT_CHECK_INT_EQ(ui_screen_raise(&screen, 0), 1);
    KT_CHECK_INT_EQ(screen.focus, 1);
    KT_CHECK_INT_EQ(ui_screen_find(&screen, 1), 1);
    KT_CHECK_INT_EQ(ui_screen_find(&screen, 2), 0);
    KT_CHECK_INT_EQ(ui_screen_top_at(&screen, 25, 25), 1);
    KT_CHECK_INT_EQ(ui_screen_raise(&screen, 99), UI_WINDOW_NONE);

    /* The focus moves to a window without moving the window. */
    KT_CHECK(ui_screen_focus(&screen, 0));
    KT_CHECK_INT_EQ(screen.focus, 0);
    KT_CHECK(!ui_screen_focus(&screen, 7));

    /* A modal window: it takes the top, the keys and the focus, and another
     * window is refused while it is up. */
    KT_CHECK_INT_EQ(ui_screen_add(&screen, 3, "dialog", &modal, 0), 2);
    KT_CHECK_INT_EQ(ui_screen_set_modal(&screen, 2), 2);
    KT_CHECK_INT_EQ(screen.modal, 2);
    KT_CHECK_INT_EQ(ui_screen_input(&screen), 2);
    KT_CHECK_INT_EQ(ui_screen_focus_next(&screen, 1), 2);
    KT_CHECK_INT_EQ(screen.focus, 2);
    KT_CHECK_INT_EQ(ui_screen_add(&screen, 4, "late", &first, 0), UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(ui_screen_raise(&screen, 0), UI_WINDOW_NONE);
    KT_CHECK(!ui_screen_focus(&screen, 0));

    /* Clearing the modal leaves the window on the screen with the keys, and
     * it is no longer modal. */
    ui_screen_clear_modal(&screen);
    KT_CHECK_INT_EQ(screen.modal, UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(screen.focus, 2);
    KT_CHECK_INT_EQ(ui_screen_input(&screen), 2);
    KT_CHECK((screen.windows[2].flags & UI_WINDOW_MODAL) == 0u);
    KT_CHECK_INT_EQ(ui_screen_add(&screen, 4, "late", &first, 0), 3);

    /* Removing the window with the keys gives them to the top of what is
     * left, and the indices above it move down. */
    KT_CHECK_INT_EQ(ui_screen_remove(&screen, 2), 0);
    KT_CHECK_INT_EQ((int)screen.count, 3);
    KT_CHECK_INT_EQ(ui_screen_find(&screen, 3), UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(ui_screen_find(&screen, 4), 2);
    KT_CHECK_INT_EQ(screen.focus, 2);
    KT_CHECK_INT_EQ(ui_screen_remove(&screen, 9), UI_WINDOW_NONE);

    /* The last window removed leaves an empty screen with no focus. */
    while (screen.count > 0u) {
        KT_CHECK_INT_EQ(ui_screen_remove(&screen, 0), 0);
    }
    KT_CHECK_INT_EQ(screen.focus, UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(screen.modal, UI_WINDOW_NONE);
    KT_CHECK_INT_EQ(ui_screen_input(&screen), UI_WINDOW_NONE);
}

/* The font of the test: a cell of eight by two pixels, 'X' is a cross, 'I'
 * is the left column and every other character is the paper. Small enough
 * that a test reads the pixels of a canvas by eye. */
static const k_u8_t s_cross[2] = {0x81u, 0x7Eu};
static const k_u8_t s_left[2] = {0x80u, 0x80u};
static const k_u8_t s_blank[2] = {0x00u, 0x00u};

static const k_u8_t *test_glyph(char character)
{
    if (character == 'X') {
        return s_cross;
    }
    if (character == 'I') {
        return s_left;
    }
    return s_blank;
}

static void test_ui_canvas(void)
{
    k_u16_t pixels[16 * 4];
    struct ui_canvas canvas;
    struct ui_rect rect;
    struct ui_font font;
    int index;

    ktest_begin("ui: the canvas fills, outlines and lays out a line of cells");

    ui_canvas_init(&canvas, 16, 4, pixels);
    ui_canvas_fill(&canvas, 0x0001u);
    for (index = 0; index < 16 * 4; index++) {
        KT_CHECK_INT_EQ(pixels[index], 0x0001);
    }

    /* A rectangle inside the canvas and one that leaves it: the part inside
     * is drawn and the canvas never sees the rest. */
    rect.x = 2;
    rect.y = 1;
    rect.w = 4;
    rect.h = 2;
    KT_CHECK_INT_EQ(ui_canvas_fill_rect(&canvas, &rect, 0x0002), 0);
    KT_CHECK_INT_EQ(pixels[1 * 16 + 2], 0x0002);
    KT_CHECK_INT_EQ(pixels[2 * 16 + 5], 0x0002);
    KT_CHECK_INT_EQ(pixels[1 * 16 + 1], 0x0001);
    KT_CHECK_INT_EQ(pixels[3 * 16 + 2], 0x0001);

    rect.x = 14;
    rect.y = 3;
    rect.w = 10;
    rect.h = 10;
    KT_CHECK_INT_EQ(ui_canvas_fill_rect(&canvas, &rect, 0x0003), 0);
    KT_CHECK_INT_EQ(pixels[3 * 16 + 15], 0x0003);
    KT_CHECK_INT_EQ(pixels[3 * 16 + 13], 0x0001);

    /* A negative origin is clipped too. */
    ui_canvas_fill(&canvas, 0);
    rect.x = -1;
    rect.y = -1;
    rect.w = 2;
    rect.h = 2;
    KT_CHECK_INT_EQ(ui_canvas_fill_rect(&canvas, &rect, 0x0004), 0);
    KT_CHECK_INT_EQ(pixels[0], 0x0004);
    KT_CHECK_INT_EQ(pixels[1], 0);

    /* A line that is wholly outside changes nothing and is not an error. */
    KT_CHECK_INT_EQ(ui_canvas_hline(&canvas, 16, 0, 4, 0x0005), 0);
    KT_CHECK_INT_EQ(ui_canvas_vline(&canvas, 0, 4, 4, 0x0005), 0);
    KT_CHECK_INT_EQ(ui_canvas_hline(NULL, 0, 0, 1, 0), -1);

    /* The frame of a rectangle of four by three: eight pixels of the twelve
     * are the outline and the middle is the paper. */
    ui_canvas_fill(&canvas, 0);
    rect.x = 1;
    rect.y = 1;
    rect.w = 4;
    rect.h = 3;
    KT_CHECK_INT_EQ(ui_canvas_frame(&canvas, &rect, 0x0006), 0);
    KT_CHECK_INT_EQ(pixels[1 * 16 + 1], 0x0006);
    KT_CHECK_INT_EQ(pixels[1 * 16 + 4], 0x0006);
    KT_CHECK_INT_EQ(pixels[3 * 16 + 1], 0x0006);
    KT_CHECK_INT_EQ(pixels[2 * 16 + 2], 0);

    /* The text: a cell of the font per character, the paper of the short
     * line, and a character that is not printable is the cell of the font of
     * the test as well (the font is the caller's and answers). */
    font.width = 8;
    font.height = 2;
    font.glyph = test_glyph;
    ui_canvas_init(&canvas, 16, 2, pixels);
    ui_canvas_fill(&canvas, 0x0010u);
    KT_CHECK_INT_EQ(ui_canvas_text(&canvas, 0, 0, "X", 2, 0x00FFu, 0x0010u, &font), 0);
    KT_CHECK_INT_EQ(pixels[0], 0x00FF); /* the left half of the cross */
    KT_CHECK_INT_EQ(pixels[7], 0x00FF); /* its right arm, same row */
    KT_CHECK_INT_EQ(pixels[8], 0x0010); /* the paper of the second cell */
    KT_CHECK_INT_EQ(pixels[1 * 16 + 0], 0x0010);
    KT_CHECK_INT_EQ(pixels[1 * 16 + 7], 0x0010);
    KT_CHECK_INT_EQ(pixels[1 * 16 + 1], 0x00FF);

    /* A cell outside the canvas is skipped, a text without a font is an
     * error. */
    KT_CHECK_INT_EQ(ui_canvas_text(&canvas, 10, 0, "X", 3, 0x00FFu, 0x0010u, &font), 0);
    KT_CHECK_INT_EQ(ui_canvas_text(&canvas, 0, 0, "X", 1, 0, 0, NULL), -1);
    KT_CHECK_INT_EQ(ui_canvas_text(&canvas, 0, 0, "X", -1, 0, 0, &font), -1);

    /* The text ends at its terminator and the cells after it are the paper,
     * whatever the bytes of the caller's buffer hold after the NUL: a line of
     * a file is a string and the tail of the buffer is not its text. */
    {
        char text[4];

        text[0] = 'X';
        text[1] = '\0';
        text[2] = 'Y';
        text[3] = '\0';

        ui_canvas_init(&canvas, 16, 2, pixels);
        ui_canvas_fill(&canvas, 0x0010u);
        KT_CHECK_INT_EQ(ui_canvas_text(&canvas, 0, 0, text, 3, 0x00FFu, 0x0010u, &font), 0);
        KT_CHECK_INT_EQ(pixels[0], 0x00FF);
        for (index = 8; index < 16; index++) {
            KT_CHECK_INT_EQ(pixels[index], 0x0010);
        }
        /* The second row of the cells after the NUL is the paper too; the
         * second row of the 'X' is its lower arm and not this check. */
        for (index = 24; index < 32; index++) {
            KT_CHECK_INT_EQ(pixels[index], 0x0010);
        }
    }
}

/* The target of the test: the rectangles of the frame as the calls of the
 * program arrive, and the pixels of the last blit. */
#define FAKE_CALLS 16

struct fake_target {
    struct ui_target target;
    int fills;
    int blits;
    struct ui_rect rects[FAKE_CALLS];
    k_u16_t colors[FAKE_CALLS];
    int last_blit;
    k_u16_t pixels[TEST_SCREEN_W];
};

static int fake_fill(void *context, const struct ui_rect *rect, k_u16_t color)
{
    struct fake_target *fake = context;

    if (fake->fills + fake->blits >= FAKE_CALLS) {
        return -1;
    }

    fake->rects[fake->fills + fake->blits] = *rect;
    fake->colors[fake->fills + fake->blits] = color;
    fake->fills++;

    return 0;
}

static int fake_blit(void *context, const struct ui_rect *rect, const k_u16_t *pixels)
{
    struct fake_target *fake = context;

    if (fake->fills + fake->blits >= FAKE_CALLS || rect->w > TEST_SCREEN_W) {
        return -1;
    }

    fake->rects[fake->fills + fake->blits] = *rect;
    fake->last_blit = rect->w * rect->h;
    fake->blits++;

    for (int index = 0; index < fake->last_blit; index++) {
        fake->pixels[index] = pixels[index];
    }

    return 0;
}

static void test_ui_frame(void)
{
    struct fake_target fake;
    struct ui_style style;
    struct ui_window window;
    struct ui_font font;
    k_u16_t scratch[64 * 2];
    int index;

    ktest_begin("ui: a frame is the paper, the border and the title of a window");

    memset(&fake, 0, sizeof(fake));
    fake.target.context = &fake;
    fake.target.fill = fake_fill;
    fake.target.blit = fake_blit;

    font.width = 8;
    font.height = 2;
    font.glyph = test_glyph;

    style.paper = 0x0000u;
    style.ink = 0xFFFFu;
    style.border = 0x1111u;
    style.focus_border = 0x2222u;
    style.title_paper = 0x0000u;
    style.title_ink = 0xFFFFu;

    memset(&window, 0, sizeof(window));
    window.id = 7;
    memcpy(window.title, "X", 2);
    window.rect.x = 4;
    window.rect.y = 4;
    window.rect.w = 3 * 8 + 2; /* three cells and the two margins */
    window.rect.h = UI_FRAME_TITLE_HEIGHT(&font) + 4;

    KT_CHECK_INT_EQ(ui_draw_frame(&fake.target, &window, &font, &style, true, scratch), 0);

    /* The paper, the four borders, the title and the separator: seven calls
     * of which one is the row of the title. */
    KT_CHECK_INT_EQ(fake.fills, 6);
    KT_CHECK_INT_EQ(fake.blits, 1);
    KT_CHECK_INT_EQ(fake.rects[0].w, window.rect.w);
    KT_CHECK_INT_EQ(fake.colors[0], style.paper);

    /* The top and the bottom lines of the border are the width of the
     * window, the left and the right ones are one pixel wide. */
    KT_CHECK_INT_EQ(fake.rects[1].y, 4);
    KT_CHECK_INT_EQ(fake.rects[1].h, 1);
    KT_CHECK_INT_EQ(fake.rects[2].y, 4 + window.rect.h - 1);
    KT_CHECK_INT_EQ(fake.rects[3].w, 1);
    KT_CHECK_INT_EQ(fake.rects[3].h, window.rect.h);
    KT_CHECK_INT_EQ(fake.rects[4].x, 4 + window.rect.w - 1);
    for (index = 1; index <= 4; index++) {
        KT_CHECK_INT_EQ(fake.colors[index], style.focus_border);
    }

    /* The title row is the width of the window at the margin of the frame,
     * and the cross of the font is in it after the padding. */
    KT_CHECK_INT_EQ(fake.rects[5].x, window.rect.x);
    KT_CHECK_INT_EQ(fake.rects[5].y, window.rect.y + UI_FRAME_MARGIN);
    KT_CHECK_INT_EQ(fake.rects[5].h, font.height);
    KT_CHECK_INT_EQ(fake.pixels[UI_FRAME_PAD], 0xFFFF);
    KT_CHECK_INT_EQ(fake.pixels[0], 0x0000);

    /* The separator sits under the title, in the colour of the frame. */
    KT_CHECK_INT_EQ(fake.rects[6].h, 1);
    KT_CHECK_INT_EQ(fake.colors[6], style.focus_border);

    /* A window without the keys keeps the plain border. */
    memset(&fake, 0, sizeof(fake));
    fake.target.context = &fake;
    fake.target.fill = fake_fill;
    fake.target.blit = fake_blit;
    KT_CHECK_INT_EQ(ui_draw_frame(&fake.target, &window, &font, &style, false, scratch), 0);
    KT_CHECK_INT_EQ(fake.colors[1], style.border);

    /* A frame without a scratch has no title and says so. */
    KT_CHECK_INT_EQ(ui_draw_frame(&fake.target, &window, &font, &style, true, NULL), -1);
    KT_CHECK_INT_EQ(ui_draw_frame(NULL, &window, &font, &style, true, scratch), -1);

    /* A window that has no room for its frame is not a window of a title:
     * the four borders and the row of the font do not fit. */
    window.rect.h = UI_FRAME_TITLE_HEIGHT(&font) - 1;
    KT_CHECK_INT_EQ(ui_draw_frame(&fake.target, &window, &font, &style, true, scratch), -1);
    window.rect.h = UI_FRAME_TITLE_HEIGHT(&font);
    window.rect.w = 2 * UI_FRAME_MARGIN;
    KT_CHECK_INT_EQ(ui_draw_frame(&fake.target, &window, &font, &style, true, scratch), -1);
}

static struct input_event key_event(k_u16_t code, k_u16_t value)
{
    struct input_event event;

    event.ms = 0;
    event.code = code;
    event.value = value;

    return event;
}

static void test_ui_keyboard(void)
{
    struct ui_keyboard keyboard;
    struct input_event event;

    ktest_begin("ui: the layout of the keyboard is the characters of the machine");

    ui_keyboard_init(&keyboard);
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_A), 'a');
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_Z), 'z');
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_0), '0');
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_SLASH), '/');
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_SPACE), ' ');
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_ENTER), 0);
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_UP), 0);

    /* Shift is the capital of a letter and the sign above a digit. */
    event = key_event(K_INPUT_KEY_SHIFT, INPUT_VALUE_PRESS);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_A), 'A');
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_1), '!');
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_SEMICOLON), ':');

    event = key_event(K_INPUT_KEY_SHIFT, INPUT_VALUE_RELEASE);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_A), 'a');

    /* Caps Lock is the letters and nothing else, and it toggles on the press
     * alone: the release of the key is not a second lock. */
    event = key_event(K_INPUT_KEY_CAPSLOCK, INPUT_VALUE_PRESS);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_B), 'B');
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_2), '2');

    event = key_event(K_INPUT_KEY_CAPSLOCK, INPUT_VALUE_RELEASE);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_B), 'B');

    /* Shift with the lock is the small letter. */
    event = key_event(K_INPUT_KEY_SHIFT, INPUT_VALUE_PRESS);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_B), 'b');
    event = key_event(K_INPUT_KEY_SHIFT, INPUT_VALUE_RELEASE);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    event = key_event(K_INPUT_KEY_CAPSLOCK, INPUT_VALUE_PRESS);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    event = key_event(K_INPUT_KEY_CAPSLOCK, INPUT_VALUE_RELEASE);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));

    /* Ctrl with a letter is the control code the console reads. */
    event = key_event(K_INPUT_KEY_CTRL, INPUT_VALUE_PRESS);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_S), 0x13);
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_A), 0x01);
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_1), 0);
    event = key_event(K_INPUT_KEY_CTRL, INPUT_VALUE_RELEASE);
    KT_CHECK(ui_keyboard_event(&keyboard, &event));
    KT_CHECK_INT_EQ(ui_keyboard_char(&keyboard, K_INPUT_KEY_S), 's');

    /* A key that is not a modifier is not consumed by the state. */
    event = key_event(K_INPUT_KEY_A, INPUT_VALUE_PRESS);
    KT_CHECK(!ui_keyboard_event(&keyboard, &event));
    KT_CHECK(!ui_keyboard_event(&keyboard, NULL));
}

void test_ui_windows(void)
{
    test_ui_rects();
    test_ui_screen();
    test_ui_canvas();
    test_ui_frame();
    test_ui_keyboard();
}
