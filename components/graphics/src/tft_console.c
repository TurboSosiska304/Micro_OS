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
 *
 * The panel itself is not this file's business: the console draws through
 * the renderer of the kernel (kernel/render.h) and the driver of the
 * display registers one at the boot. A board without a display registers
 * none, and then the dialogue simply has no copy on a screen - the serial
 * port carries it, and nothing here has to say that a screen is missing.
 */
#include "graphics/tft_console.h"

#include <string.h>

#include "graphics/font.h"
#include "graphics/text_wrap.h"
#include "kernel/lock.h"
#include "kernel/log.h"
#include "kernel/panic.h"
#include "kernel/render.h"
#include "kernel/sched.h"
#include "kernel/task.h"

/*
 * How much of the picture the dialogue fits into depends on its turn:
 * the portrait shows 22 columns of 26 rows, the landscape 40 of 15. The
 * line buffers are made for the biggest picture the console is meant
 * for - the long side of the panel of the board (240) - and the counts
 * themselves are of the view the panel is showing right now and are
 * recomputed when the picture is turned (console_layout). A bigger
 * renderer gets its dialogue cut at the edges rather than the board
 * asking for more memory.
 */
#define TFT_PICTURE_MAX 240
#define TFT_COLUMNS_MAX ((TFT_PICTURE_MAX / (FONT_WIDTH + 1))) /* the widest picture: 40 */
#define TFT_ROWS_MAX ((TFT_PICTURE_MAX / (FONT_HEIGHT + 2)))   /* the tallest picture: 26 */
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
 * The lines are written by the task that prints and read by the task that
 * draws: the two of them never hold the same line at the same time. The
 * mutex is recursive because the paths that redraw the dialogue call the
 * helpers that draw a row.
 */
static k_mutex_t s_lock = K_MUTEX_INITIALIZER;

/* The screen shows something older than the lines that were written. */
static volatile bool s_dirty;

static void console_lock(void)
{
    if (k_mutex_lock(&s_lock) != 0) {
        /* No memory for the lock is not a condition the console can work
         * around: half of the dialogue would be drawn over the other. */
        KERNEL_PANIC("tft-console: cannot take the lock of the console");
    }
}

static void console_unlock(void)
{
    (void)k_mutex_unlock(&s_lock);
}

/*
 * Every painting of the console goes through the three calls below. The
 * renderer is asked for on each of them and never kept: the panel may come
 * up after the console, and a board without one answers by doing nothing,
 * quietly. That is why the "there is no display" case lives here and the
 * drawing code reads as if a picture were always there.
 */
static void picture_clear(k_u16_t color)
{
    const struct k_renderer *render = k_render_get();

    if (render != NULL) {
        render->clear(render->ctx, color);
    }
}

static void picture_text(int x, int y, const char *text, k_u16_t color)
{
    const struct k_renderer *render = k_render_get();

    if (render != NULL) {
        render->text(render->ctx, x, y, text, color);
    }
}

/* A renderer that draws directly has no flush hook: nothing to send. */
static void picture_flush(void)
{
    const struct k_renderer *render = k_render_get();

    if (render != NULL && render->flush != NULL) {
        render->flush(render->ctx);
    }
}

/*
 * The counts of columns and rows of the picture as it is turned right now,
 * cut to the size the line buffers have room for. Without a renderer the
 * dialogue keeps the widest shape: a screen may still appear, and the
 * lines are then already cut for the room there is.
 */
static void console_measure(int *columns, int *rows)
{
    const struct k_renderer *render = k_render_get();
    int width = TFT_PICTURE_MAX;
    int height = TFT_PICTURE_MAX;

    if (render != NULL) {
        width = render->width(render->ctx);
        height = render->height(render->ctx);
    }

    *columns = width / (FONT_WIDTH + 1);
    *rows = height / (FONT_HEIGHT + 2);

    if (*columns > TFT_COLUMNS_MAX) {
        *columns = TFT_COLUMNS_MAX;
    }
    if (*columns < 1) {
        *columns = 1;
    }
    if (*rows > TFT_ROWS_MAX) {
        *rows = TFT_ROWS_MAX;
    }
    if (*rows < 1) {
        *rows = 1;
    }
}

/* The caller holds the lock of the console. */
static void draw(void)
{
    int row;
    char piece[TFT_LINE_MAX];
    int at;
    int index = 0;

    picture_clear(0x0000);

    for (row = 0; row < s_line_count; row++) {
        picture_text(0, row * (FONT_HEIGHT + 2), s_lines[row], 0xFFFF);
    }

    /*
     * The line that is being written: it is cut into the rows by the
     * same width as the lines of the history, so a long command is
     * read to the end instead of running past the right edge.
     */
    at = s_line_count;

    while (at < s_rows && text_wrap_take(piece, sizeof(piece), s_current, s_columns, index) > 0) {
        picture_text(0, at * (FONT_HEIGHT + 2), piece, 0x07E0);
        at++;
        index++;
    }

    picture_flush();
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
static void push_one_row(const char *text)
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
 * A line of the dialogue: the tail of a line that is wider than the
 * picture goes to the row below - cut by the width the picture has
 * right now, so the same line is two rows of the portrait and one of
 * the landscape. An empty line is still a line.
 */
static void push_line(const char *text)
{
    char piece[TFT_LINE_MAX];
    int row = 0;

    while (text_wrap_take(piece, sizeof(piece), text, s_columns, row) > 0) {
        push_one_row(piece);
        row++;
    }

    if (row == 0) {
        push_one_row("");
    }
}

/*
 * The dialogue cut down to the picture as it is turned right now: the
 * counts of columns and rows of it, and the lines cut to the new width -
 * the oldest lines go, and the line that is being written loses what is
 * beyond the new edge. The caller holds the lock of the console.
 */
static void console_layout(void)
{
    console_measure(&s_columns, &s_rows);

    /*
     * The lines are cut into the new width: the tail of a line of the
     * wide picture goes to the row below on the narrow one, and the
     * rows that do not fit leave from the top - the same as scrolling.
     * The line that is being written is not touched: it is drawn by
     * the rows of the width as well, and its bytes stay where they are.
     */
    {
        static char old[TFT_ROWS_MAX][TFT_LINE_MAX];
        int old_count = s_line_count;
        int i;

        memcpy(old, s_lines, sizeof(old));
        s_line_count = 0;

        for (i = 0; i < old_count; i++) {
            char piece[TFT_LINE_MAX];
            int row = 0;

            while (text_wrap_take(piece, sizeof(piece), old[i], s_columns, row) > 0) {
                push_one_row(piece);
                row++;
            }

            if (row == 0) {
                push_one_row("");
            }
        }
    }
}

/*
 * Has the picture changed its shape since the dialogue was cut for it?
 * The turn of the panel does exactly that, and the console is not told
 * about it: the driver of the display knows nothing about a console, so
 * the console looks at the picture it draws through.
 */
static bool console_shape_changed(void)
{
    int columns;
    int rows;

    console_measure(&columns, &rows);

    return columns != s_columns || rows != s_rows;
}

/*
 * The task that brings the screen up to date. It sleeps most of its life
 * and only looks at the screen when the printing has said that it is out
 * of date - or when the panel was turned under it.
 */
static void flush_task(void *arg)
{
    K_UNUSED(arg);

    for (;;) {
        k_sched_sleep_ms(TFT_FLUSH_MS);

        console_lock();

        if (!s_started) {
            console_unlock();
            continue;
        }

        /*
         * A turned panel needs the dialogue cut another way: the same
         * lines are more rows in the portrait than in the landscape.
         */
        if (console_shape_changed()) {
            console_layout();
            s_dirty = true;
        }

        if (!s_dirty) {
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
    picture_clear(0x0000);
    picture_text(0, 0, "MICRO OS", 0x07E0);
    picture_flush();
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

        /*
         * The buffer of the line is the widest line there is: when it
         * is full and one more letter comes, what is in it goes to the
         * rows below and the letter starts the next piece - none of a
         * long line is lost past the right edge.
         */
        if (s_cursor == TFT_LINE_MAX - 1) {
            push_line(s_current);
            s_cursor = 0;
            s_current[0] = '\0';
        }

        s_current[s_cursor++] = c;
        s_current[s_cursor] = '\0';
    }

    console_unlock();

    /* The lines moved: the task that draws will bring the screen up to date. */
    s_dirty = true;
}
