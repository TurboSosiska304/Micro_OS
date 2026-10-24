/*
 * Micro OS — the console on the display of the board.
 *
 * The output of the shell is mirrored to the screen: the last lines of the
 * dialogue are shown, the text is written with the small font of the board.
 *
 * The task that prints does not draw. It writes the lines down and says
 * that the screen is out of date, and one task of its own, of a low
 * priority, brings the screen up to date at most ten times a second. A
 * listing of fifty lines is one redraw instead of fifty, and the board
 * does not stand still in the middle of the dialogue: redrawing after
 * every line costs a clear of the panel and a flush of its whole memory
 * (64 KB over the SPI) per line.
 */
#include "graphics/tft_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "graphics/font.h"
#include "kernel/log.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kshell/kshell.h"

/*
 * How much of the picture the dialogue fits into depends on its turn:
 * the portrait shows 22 columns of 26 rows, the landscape 40 of 15. The
 * line buffers are made for the biggest view there is, the counts
 * themselves are of the view the panel is showing right now and are
 * recomputed when the picture is turned (console_layout).
 */
#define TFT_COLUMNS_MAX ((DISPLAY_HEIGHT / (FONT_WIDTH + 1))) /* the widest picture: 40 */
#define TFT_ROWS_MAX ((DISPLAY_HEIGHT / (FONT_HEIGHT + 2)))   /* the tallest picture: 26 */
#define TFT_LINE_MAX (TFT_COLUMNS_MAX + 1)

/* The screen is brought up to date at most ten times a second. */
#define TFT_FLUSH_MS 100

/* The priority of the task that draws: the task that prints comes first. */
#define TFT_FLUSH_PRIO 3

/* The lines of the dialogue, the oldest is overwritten. */
static char s_lines[TFT_ROWS_MAX][TFT_LINE_MAX];
static int s_line_count;
static int s_cursor;
static char s_current[TFT_LINE_MAX];
static bool s_started;

/* The view the panel is showing: the counts of columns and rows of it. */
static int s_columns = TFT_COLUMNS_MAX;
static int s_rows = TFT_ROWS_MAX;

/*
 * The test picture of the `display draw` command owns the screen: while
 * this is set the task that draws must not take it back — the dialogue
 * returns when the command says so.
 */
static volatile bool s_demo;

/*
 * The lines are written by the task that prints and read by the task that
 * draws: the two of them never hold the same line at the same time.
 */
static SemaphoreHandle_t s_lock;

/* The screen shows something older than the lines that were written. */
static volatile bool s_dirty;

static void console_lock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    }
}

static void console_unlock(void)
{
    if (s_lock != NULL) {
        (void)xSemaphoreGiveRecursive(s_lock);
    }
}

/* The caller holds the lock of the console. */
static void draw(void)
{
    int row;

    drv_display_clear(0x0000);

    for (row = 0; row < s_line_count; row++) {
        drv_display_text(0, row * (FONT_HEIGHT + 2), s_lines[row], 0xFFFF);
    }

    /* The line that is being written. */
    drv_display_text(0, s_line_count * (FONT_HEIGHT + 2), s_current, 0x07E0);

    (void)drv_display_flush();
}

void tft_console_redraw(void)
{
    /*
     * The drawing is done by the task that owns the screen: this only
     * says that what is on the screen is older than the lines.
     */
    s_dirty = true;
}

/*
 * Copy a line into the buffer: the visible part of it, the rest of the
 * buffer is spaces (the line is a line however wide the picture is).
 */
static void copy_line(char *dst, const char *text)
{
    int i;

    for (i = 0; i < s_columns; i++) {
        dst[i] = (text != NULL && text[i] != '\0') ? text[i] : ' ';
    }
    dst[s_columns] = '\0';

    for (i = s_columns + 1; i < TFT_LINE_MAX; i++) {
        dst[i] = ' ';
    }
}

/* The caller holds the lock of the console. */
static void push_line(const char *text)
{
    if (s_line_count < s_rows) {
        s_line_count++;
    } else {
        int i;

        for (i = 0; i < s_rows - 1; i++) {
            memcpy(s_lines[i], s_lines[i + 1], TFT_LINE_MAX);
        }
    }

    copy_line(s_lines[s_line_count - 1], text);
}

/*
 * The counts of columns and rows of the picture as it is turned right
 * now, and the dialogue cut down to fit it: the oldest lines go, and
 * the line that is being written loses what is beyond the new edge.
 * The caller holds the lock of the console.
 */
static void console_layout(void)
{
    s_columns = drv_display_width() / (FONT_WIDTH + 1);
    s_rows = drv_display_height() / (FONT_HEIGHT + 2);

    if (s_columns > TFT_COLUMNS_MAX) {
        s_columns = TFT_COLUMNS_MAX;
    }
    if (s_columns < 1) {
        s_columns = 1;
    }
    if (s_rows > TFT_ROWS_MAX) {
        s_rows = TFT_ROWS_MAX;
    }
    if (s_rows < 1) {
        s_rows = 1;
    }

    while (s_line_count > s_rows) {
        int i;

        for (i = 0; i < s_line_count - 1; i++) {
            memcpy(s_lines[i], s_lines[i + 1], TFT_LINE_MAX);
        }
        s_line_count--;
    }

    if (s_cursor > s_columns) {
        s_cursor = s_columns;
        s_current[s_cursor] = '\0';
    }
}

/*
 * The task that brings the screen up to date. It sleeps most of its life
 * and only looks at the screen when the printing has said that it is out
 * of date.
 */
static void flush_task(void *arg)
{
    K_UNUSED(arg);

    for (;;) {
        k_sched_sleep_ms(TFT_FLUSH_MS);

        /*
         * The check of the screen is under the lock: a command that has
         * taken the screen for itself (the `display draw`) says so
         * before it takes the lock, and no redraw can slip past the
         * check and land on its picture.
         */
        console_lock();

        if (!s_started || !s_dirty || s_demo) {
            console_unlock();
            continue;
        }

        draw();
        s_dirty = false;
        console_unlock();
    }
}

void tft_console_start(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateRecursiveMutex();
        if (s_lock == NULL) {
            kerror("display: the lock of the console cannot be made");
            return;
        }
    }

    console_lock();

    memset(s_lines, 0, sizeof(s_lines));
    s_line_count = 0;
    s_cursor = 0;
    s_current[0] = '\0';
    console_layout();
    s_started = true;

    console_unlock();

    /* The panel shows the name of the system at once. */
    console_lock();
    drv_display_clear(0x0000);
    drv_display_text(0, 0, "MICRO OS", 0x07E0);
    (void)drv_display_flush();
    console_unlock();

    s_dirty = false;

    /*
     * The task that draws. There is only one, and it is only made once:
     * the console of the board is a singleton, like the board.
     */
    if (task_self() != NULL) {
        struct task *task = task_create("tft-flush", flush_task, NULL, TFT_FLUSH_PRIO);

        if (task == NULL) {
            kerror("display: the task that draws the console cannot be made");
        }
    }
}

/*
 * The test picture: the graphics of the driver in a form a person can
 * check with their eyes — a frame, the three colours a pixel is made
 * of, the diagonals that make a wrong turn plain to see, and the
 * colour bars along the bottom. The caller holds the lock of the
 * console and flushes the picture itself.
 */
static void demo_picture(void)
{
    static const uint16_t palette[8] = {0xF800, 0xFC00, 0xFFE0, 0x07E0,
                                        0x07FF, 0x001F, 0xF81F, 0xFFFF};
    int w = drv_display_width();
    int h = drv_display_height();
    int cell = ((w < h) ? w : h) / 8;
    int i;

    if (cell < 4) {
        cell = 4;
    }

    drv_display_rect(0, 0, w, h, 0xFFFF);

    /* red, green, blue */
    drv_display_fill_rect(cell / 2, cell / 2, cell, cell, 0xF800);
    drv_display_fill_rect(cell * 5 / 2, cell / 2, cell, cell, 0x07E0);
    drv_display_fill_rect(cell * 7 / 2, cell / 2, cell, cell, 0x001F);

    /* the diagonals: a turn of the picture that went wrong is plain here */
    drv_display_line(0, 0, w - 1, h - 1, 0xFFFF);
    drv_display_line(w - 1, 0, 0, h - 1, 0xFFFF);

    /* the colour bars along the bottom */
    for (i = 0; i < 8; i++) {
        int x0 = i * w / 8;
        int x1 = (i + 1) * w / 8;

        drv_display_fill_rect(x0 + 1, h - cell - 1, x1 - x0 - 2, cell, palette[i]);
    }
}

/*
 * The offset of the visible area of the panel, the turn of the picture
 * and the test picture of the graphics:
 *
 *   display                        what the panel shows right now
 *   display <x> <y>                the offset of the visible area
 *   display rotate <0|90|180|270>  the picture the other way round
 *   display draw [seconds]         a test picture for a few seconds
 */
static int cmd_display(int argc, char **argv)
{
    int x = 0;
    int y = 0;

    if (argc > 1 && strcmp(argv[1], "rotate") == 0) {
        char *end = NULL;
        long degrees;

        if (argc < 3) {
            kshell_printf("usage: display rotate <0|90|180|270>\n");
            return KSHELL_OK;
        }

        degrees = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || (degrees % 90) != 0) {
            kshell_printf("display: the turn has to be one of 0, 90, 180, 270\n");
            return KSHELL_OK;
        }

        drv_display_set_rotation((int)(degrees / 90));

        console_lock();
        console_layout();
        console_unlock();
        tft_console_redraw();

        kshell_printf("display rotation: %d, picture %dx%d\n", drv_display_rotation() * 90,
                      drv_display_width(), drv_display_height());
        return KSHELL_OK;
    }

    if (argc > 1 && strcmp(argv[1], "draw") == 0) {
        long seconds = 6;

        if (argc > 2) {
            char *end = NULL;

            seconds = strtol(argv[2], &end, 10);
            if (end == argv[2] || *end != '\0' || seconds < 0 || seconds > 60) {
                kshell_printf("usage: display draw [0..60 seconds]\n");
                return KSHELL_OK;
            }
        }

        /*
         * The picture takes the screen: the flag says so before the lock
         * is taken (the task that draws checks it under the lock), and
         * the lock waits out a redraw that is already under way. The
         * dialogue comes back when the command is done with it.
         */
        s_demo = true;
        console_lock();
        demo_picture();
        (void)drv_display_flush();
        console_unlock();

        if (seconds > 0) {
            k_sched_sleep_ms((k_u32_t)seconds * 1000u);
        }

        s_demo = false;
        tft_console_redraw();
        return KSHELL_OK;
    }

    drv_display_get_offset(&x, &y);

    if (argc > 1) {
        char *end = NULL;
        long value = strtol(argv[1], &end, 10);

        if (end == argv[1] || *end != '\0') {
            kshell_printf("usage: display [<x> <y>] | rotate <0|90|180|270> | draw [seconds]\n");
            return KSHELL_OK;
        }

        x = (int)value;
        y = (argc > 2) ? (int)strtol(argv[2], NULL, 10) : y;

        drv_display_set_offset(x, y);
        tft_console_redraw();
    }

    kshell_printf("display offset: x=%d y=%d\n", x, y);
    kshell_printf("display rotation: %d, picture %dx%d\n", drv_display_rotation() * 90,
                  drv_display_width(), drv_display_height());

    return KSHELL_OK;
}

int tft_console_register(void)
{
    return kshell_register("display", "the offset and the turn of the picture on the screen",
                           cmd_display);
}

void tft_console_write(const char *data, k_size_t len)
{
    k_size_t i;

    if (!s_started || data == NULL) {
        return;
    }

    console_lock();

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\n') {
            s_current[s_cursor] = '\0';
            push_line(s_current);
            s_cursor = 0;
            s_current[0] = '\0';
            continue;
        }

        if (c == '\r') {
            s_cursor = 0;
            s_current[0] = '\0';
            continue;
        }

        if (s_cursor < s_columns) {
            s_current[s_cursor++] = c;
            s_current[s_cursor] = '\0';
        }
    }

    console_unlock();

    /* The lines moved: the task that draws will bring the screen up to date. */
    s_dirty = true;
}
