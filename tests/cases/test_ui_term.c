/*
 * Micro OS — the grid of a terminal window and the clock of the panel
 * (ADR 0054, docs/desktop.md).
 *
 * `/bin/term` is two halves: the program, which owns a window and a child
 * process, and this grid, which is the portable half of the computer of a
 * terminal - the bytes of a program in cells, the line endings, the scroll and
 * the damage of a repaint. The half that draws a screen is not here, so the
 * text a person reads in the window is held without a window.
 *
 * The clock is the same kind of thing: the format of the theme and the hour of
 * a moment of the wall clock, and no machine.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "ui/clock.h"
#include "ui/term.h"

#define TEST_COLS 20
#define TEST_ROWS 5

static char g_cells[TEST_COLS * TEST_ROWS];

static char cell(const struct ui_term *term, int x, int y)
{
    return term->cells[y * term->cols + x];
}

/* The text of a row of the grid, the trailing spaces cut. */
static void row_text(const struct ui_term *term, int y, char *out)
{
    int x;
    int length = 0;

    for (x = 0; x < term->cols; x++) {
        out[x] = cell(term, x, y);
    }

    out[term->cols] = '\0';

    for (x = term->cols - 1; x >= 0; x--) {
        if (out[x] != ' ') {
            length = x + 1;
            break;
        }
    }

    out[length] = '\0';
}

static void test_ui_term_text(void)
{
    struct ui_term term;
    char row[TEST_COLS + 1];

    ktest_begin("ui: the text of a terminal is the cells of its grid");

    ui_term_init(&term, TEST_COLS, TEST_ROWS, g_cells);
    KT_CHECK_INT_EQ(term.cursor_x, 0);
    KT_CHECK_INT_EQ(term.cursor_y, 0);

    (void)ui_term_put(&term, "hello", 5);
    KT_CHECK_INT_EQ(term.cursor_x, 5);
    row_text(&term, 0, row);
    KT_CHECK_STR_EQ(row, "hello");

    (void)ui_term_put(&term, "\rworld", 6);
    row_text(&term, 0, row);
    KT_CHECK_STR_EQ(row, "world");

    (void)ui_term_put(&term, "\nsecond", 7);
    KT_CHECK_INT_EQ(term.cursor_y, 1);
    row_text(&term, 1, row);
    KT_CHECK_STR_EQ(row, "second");

    /* A backspace moves the cursor and not the cells: the caller writes the
     * space that erases the letter. Two of them and a space erase the 'n'. */
    (void)ui_term_put(&term, "\b\b \b", 4);
    row_text(&term, 1, row);
    KT_CHECK_STR_EQ(row, "seco d");
    KT_CHECK_INT_EQ(term.cursor_x, 4);

    /* A tab goes to the next multiple of eight columns. */
    ui_term_clear(&term);
    (void)ui_term_put(&term, "a\tb", 3);
    KT_CHECK_INT_EQ(cell(&term, 1, 0), ' ');
    KT_CHECK_INT_EQ(cell(&term, 7, 0), ' ');
    KT_CHECK_INT_EQ(cell(&term, 8, 0), 'b');

    /* A control code that is not a letter of the screen leaves no cell. */
    ui_term_clear(&term);
    (void)ui_term_put(&term, "a\ab\f", 5);
    row_text(&term, 0, row);
    KT_CHECK_STR_EQ(row, "ab");
}

static void test_ui_term_wrap(void)
{
    struct ui_term term;
    char row[TEST_COLS + 1];

    ktest_begin("ui: a full row of a terminal wraps and does not scroll");

    ui_term_init(&term, 8, 3, g_cells);

    (void)ui_term_put(&term, "12345678", 8);
    KT_CHECK_INT_EQ(term.cursor_x, 7);
    KT_CHECK_INT_EQ(term.cursor_y, 0);
    KT_CHECK(term.wrap_next);
    KT_CHECK_INT_EQ(term.scrolled, 0u);

    (void)ui_term_put(&term, "9", 1);
    KT_CHECK_INT_EQ(term.cursor_x, 1);
    KT_CHECK_INT_EQ(term.cursor_y, 1);
    KT_CHECK_INT_EQ(cell(&term, 0, 0), '1');
    KT_CHECK_INT_EQ(cell(&term, 7, 0), '8');
    KT_CHECK_INT_EQ(cell(&term, 0, 1), '9');

    /* A carriage return forgets the deferred wrap: a status line written
     * over itself stays one row. */
    (void)ui_term_put(&term, "\rstatus", 7);
    row_text(&term, 1, row);
    KT_CHECK_STR_EQ(row, "status");
    KT_CHECK(!term.wrap_next);
}

static void test_ui_term_scroll(void)
{
    struct ui_term term;
    char row[TEST_COLS + 1];
    int y;

    ktest_begin("ui: a terminal scrolls a row that leaves the top");

    ui_term_init(&term, 10, 3, g_cells);

    for (y = 1; y <= 4; y++) {
        char line[4];

        line[0] = (char)('0' + y);
        line[1] = '\n';
        (void)ui_term_put(&term, line, 2);
    }

    /* Four lines of a grid of three rows and the line ends after them: the two
     * first lines leave the top of the grid, the cursor is on the last row of
     * the third scroll, and the row of the fourth line is the one above it. */
    KT_CHECK_INT_EQ(term.scrolled, 2u);
    row_text(&term, 0, row);
    KT_CHECK_STR_EQ(row, "3");
    row_text(&term, 1, row);
    KT_CHECK_STR_EQ(row, "4");
    row_text(&term, 2, row);
    KT_CHECK_STR_EQ(row, "");
    KT_CHECK_INT_EQ(term.cursor_y, 2);
    KT_CHECK_INT_EQ(term.cursor_x, 0);
}

static void test_ui_term_escape(void)
{
    struct ui_term term;
    char row[TEST_COLS + 1];

    ktest_begin("ui: the sequences of the screen of a console");

    ui_term_init(&term, 10, 3, g_cells);

    (void)ui_term_put(&term, "one\ntwo", 7);
    KT_CHECK_INT_EQ(term.cursor_y, 1);

    /* The cursor at a cell, 1-based, and a letter there. */
    (void)ui_term_put(&term, "\033[1;3HX", 7);
    KT_CHECK_INT_EQ(cell(&term, 2, 0), 'X');
    KT_CHECK_INT_EQ(cell(&term, 0, 0), 'o');
    KT_CHECK_INT_EQ(term.cursor_x, 3);

    /* ESC[K erases to the end of the row and leaves the text before it. */
    (void)ui_term_put(&term, "\033[1;2H\033[K", 9);
    row_text(&term, 0, row);
    KT_CHECK_STR_EQ(row, "o");

    /* ESC[2J is the whole grid blank and the cursor home. */
    (void)ui_term_put(&term, "\033[2J", 4);
    row_text(&term, 0, row);
    KT_CHECK_STR_EQ(row, "");
    row_text(&term, 1, row);
    KT_CHECK_STR_EQ(row, "");
    KT_CHECK_INT_EQ(term.cursor_x, 0);
    KT_CHECK_INT_EQ(term.cursor_y, 0);

    /* A sequence this version does not know leaves no trace and no letter:
     * the ?25l that hides a cursor of a full-screen program. */
    (void)ui_term_put(&term, "\033[?25lA", 7);
    row_text(&term, 0, row);
    KT_CHECK_STR_EQ(row, "A");
    KT_CHECK_INT_EQ(term.cursor_x, 1);
}

static void test_ui_term_damage(void)
{
    struct ui_term term;
    int x;
    int y;
    int w;
    int h;

    ktest_begin("ui: the damage of a terminal is the rows that changed");

    ui_term_init(&term, 20, 5, g_cells);

    /* The init paints the whole grid: nothing of the old picture is left. */
    KT_CHECK(ui_term_take_damage(&term, &x, &y, &w, &h));
    KT_CHECK_INT_EQ(x, 0);
    KT_CHECK_INT_EQ(y, 0);
    KT_CHECK_INT_EQ(w, 20);
    KT_CHECK_INT_EQ(h, 5);
    KT_CHECK(!ui_term_take_damage(&term, &x, &y, &w, &h));

    /* A line on the third row is the third row and nothing else. */
    (void)ui_term_put(&term, "\033[3;1Hhello", 11);
    KT_CHECK(ui_term_take_damage(&term, &x, &y, &w, &h));
    KT_CHECK_INT_EQ(x, 0);
    KT_CHECK_INT_EQ(y, 2);
    KT_CHECK_INT_EQ(w, 5);
    KT_CHECK_INT_EQ(h, 1);

    /* Two keys of one turn are one rectangle of their two rows. */
    (void)ui_term_put(&term, "a", 1);
    (void)ui_term_put(&term, "\033[5;2Hb", 7);
    KT_CHECK(ui_term_take_damage(&term, &x, &y, &w, &h));
    KT_CHECK_INT_EQ(x, 1);
    KT_CHECK_INT_EQ(y, 2);
    KT_CHECK_INT_EQ(w, 5);
    KT_CHECK_INT_EQ(h, 3);

    /* A scroll is every row of the grid: the one damage a terminal cannot make
     * small. */
    KT_CHECK(!ui_term_take_damage(&term, &x, &y, &w, &h));
    (void)ui_term_put(&term, "\n\n\n\n\n", 5);
    KT_CHECK(ui_term_take_damage(&term, &x, &y, &w, &h));
    KT_CHECK_INT_EQ(w, 20);
    KT_CHECK_INT_EQ(h, 5);
    KT_CHECK(term.scrolled >= 1u);
}

static void test_ui_clock_format(void)
{
    struct ui_clock clock;
    char text[UI_CLOCK_TEXT_MAX];

    ktest_begin("ui: the clock of the panel is the format of the theme");

    ui_clock_default(&clock);
    KT_CHECK(!clock.twelve);
    KT_CHECK_INT_EQ(clock.offset_minutes, 0);

    /* 2026-11-24T15:04:05Z is 1763996645; the second of a day is what the
     * clock draws. */
    ui_clock_text(&clock, 15L * 3600L + 4L * 60L + 5L, text, sizeof(text));
    KT_CHECK_STR_EQ(text, "15:04:05");

    /* Twelve hours, with the midnight and the noon of a clock. */
    ui_clock_line(&clock, "clock=12h");
    KT_CHECK(clock.twelve);
    ui_clock_text(&clock, 15L * 3600L + 4L * 60L + 5L, text, sizeof(text));
    KT_CHECK_STR_EQ(text, "3:04:05 PM");
    ui_clock_text(&clock, 0, text, sizeof(text));
    KT_CHECK_STR_EQ(text, "12:00:00 AM");
    ui_clock_text(&clock, 12L * 3600L, text, sizeof(text));
    KT_CHECK_STR_EQ(text, "12:00:00 PM");
    ui_clock_text(&clock, 9L * 3600L + 5L * 60L, text, sizeof(text));
    KT_CHECK_STR_EQ(text, "9:05:00 AM");

    /* The zone is minutes east of UTC, and the negative ones wrap the day. */
    ui_clock_line(&clock, "clock=24h");
    KT_CHECK(ui_clock_line(&clock, "utc_offset=180"));
    KT_CHECK_INT_EQ(clock.offset_minutes, 180);
    ui_clock_text(&clock, 23L * 3600L + 30L * 60L, text, sizeof(text));
    KT_CHECK_STR_EQ(text, "02:30:00");
    KT_CHECK(ui_clock_line(&clock, "utc_offset=-300"));
    ui_clock_text(&clock, 2L * 3600L, text, sizeof(text));
    KT_CHECK_STR_EQ(text, "21:00:00");

    /* A line of another key of /etc/console is not the clock, and a value
     * this version cannot read leaves the field as it was. */
    KT_CHECK(!ui_clock_line(&clock, "theme=light"));
    KT_CHECK(!ui_clock_line(&clock, "a line that is not a setting"));
    KT_CHECK(ui_clock_line(&clock, "clock=13h"));
    KT_CHECK(!clock.twelve);
    KT_CHECK(ui_clock_line(&clock, "utc_offset=nowhere"));
    KT_CHECK_INT_EQ(clock.offset_minutes, -300);
    KT_CHECK(ui_clock_line(&clock, "clock=24h\n"));
    KT_CHECK(!clock.twelve);
}

void test_ui_term(void)
{
    test_ui_term_text();
    test_ui_term_wrap();
    test_ui_term_scroll();
    test_ui_term_escape();
    test_ui_term_damage();
    test_ui_clock_format();
}
