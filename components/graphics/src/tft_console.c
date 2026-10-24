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

#define TFT_COLUMNS ((DISPLAY_WIDTH / (FONT_WIDTH + 1)))
#define TFT_ROWS ((DISPLAY_HEIGHT / (FONT_HEIGHT + 2)))
#define TFT_LINE_MAX (TFT_COLUMNS + 1)

/* The screen is brought up to date at most ten times a second. */
#define TFT_FLUSH_MS 100

/* The priority of the task that draws: the task that prints comes first. */
#define TFT_FLUSH_PRIO 3

/* The lines of the dialogue, the oldest is overwritten. */
static char s_lines[TFT_ROWS][TFT_LINE_MAX];
static int s_line_count;
static int s_cursor;
static char s_current[TFT_LINE_MAX];
static bool s_started;

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
 * Copy a line into the buffer, the rest is filled up.
 */
static void copy_line(char *dst, const char *text)
{
    int i;

    for (i = 0; i < TFT_COLUMNS; i++) {
        dst[i] = (text != NULL && text[i] != '\0') ? text[i] : ' ';
    }
    dst[TFT_COLUMNS] = '\0';
}

/* The caller holds the lock of the console. */
static void push_line(const char *text)
{
    if (s_line_count < TFT_ROWS) {
        s_line_count++;
    } else {
        int i;

        for (i = 0; i < TFT_ROWS - 1; i++) {
            memcpy(s_lines[i], s_lines[i + 1], TFT_LINE_MAX);
        }
    }

    copy_line(s_lines[s_line_count - 1], text);
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

        if (!s_started || !s_dirty) {
            continue;
        }

        console_lock();
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
 * The offset of the visible area of the panel. The glass of the 1.14" panel
 * shows only a part of the memory of the controller, and the values differ
 * between the models, so they can be changed while the board runs.
 */
static int cmd_display(int argc, char **argv)
{
    int x = 0;
    int y = 0;

    drv_display_get_offset(&x, &y);

    if (argc > 1) {
        x = (int)strtol(argv[1], NULL, 10);
        y = (argc > 2) ? (int)strtol(argv[2], NULL, 10) : y;

        drv_display_set_offset(x, y);
        tft_console_redraw();
    }

    kshell_printf("display offset: x=%d y=%d\n", x, y);

    return KSHELL_OK;
}

/*
 * The heart of the command "heart": a picture of thirteen columns and
 * twelve rows, every '#' one square of the colour of the heart.
 */
static const char *const s_heart[] = {
    "  ###   ###  ", " ##### ##### ", "#############", "#############",
    "#############", "#############", " ########### ", "  #########  ",
    "   #######   ", "    #####    ", "     ###     ", "      #      ",
};

#define HEART_ROWS ((int)(sizeof(s_heart) / sizeof(s_heart[0])))
#define HEART_COLS 13

/* One frame of the beat: the picture in the middle of the black screen. */
static void heart_beat(int cell, uint16_t color)
{
    int x0 = (DISPLAY_WIDTH - HEART_COLS * cell) / 2;
    int y0 = (DISPLAY_HEIGHT - HEART_ROWS * cell) / 2;
    int row;
    int col;

    (void)drv_display_clear(0x0000);

    for (row = 0; row < HEART_ROWS; row++) {
        for (col = 0; col < HEART_COLS; col++) {
            if (s_heart[row][col] == '#') {
                (void)drv_display_fill_rect(x0 + col * cell, y0 + row * cell, cell, cell, color);
            }
        }
    }

    (void)drv_display_flush();
}

/*
 * The command "heart": the heart beats on the screen for a moment, and the
 * dialogue of the shell comes back after it - the console keeps its lines
 * and draws them again.
 */
static int cmd_heart(int argc, char **argv)
{
    int beat;

    (void)argc;
    (void)argv;

    /*
     * Every frame is drawn under the lock of the console, so the task that
     * draws the dialogue cannot walk into the middle of it; the lock is
     * dropped between the frames, so a line of the log waits one frame at
     * most.
     */
    for (beat = 0; beat < 4; beat++) {
        console_lock();
        heart_beat(5, 0xF800);
        console_unlock();
        k_sched_sleep_ms(170);

        console_lock();
        heart_beat(6, 0xF800);
        console_unlock();
        k_sched_sleep_ms(170);
    }

    /* The dialogue of the shell is older than the screen: it comes back. */
    tft_console_redraw();

    return KSHELL_OK;
}

int tft_console_register(void)
{
    int rc = kshell_register("display", "the offset of the image on the screen", cmd_display);

    rc |= kshell_register("heart", "a heart on the screen for a moment", cmd_heart);

    return rc;
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

        if (s_cursor < TFT_COLUMNS) {
            s_current[s_cursor++] = c;
            s_current[s_cursor] = '\0';
        }
    }

    console_unlock();

    /* The lines moved: the task that draws will bring the screen up to date. */
    s_dirty = true;
}
