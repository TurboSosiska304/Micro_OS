/*
 * Micro OS — the console on the display of the board.
 *
 * The output of the shell is mirrored to the screen: the last lines of the
 * dialogue are shown, the text is written with the small font of the board.
 */
#include "graphics/tft_console.h"

#include <string.h>

#include "drivers/display.h"
#include "graphics/font.h"
#include "kernel/log.h"
#include "kshell/kshell.h"

#define TFT_COLUMNS ((DISPLAY_WIDTH / (FONT_WIDTH + 1)))
#define TFT_ROWS ((DISPLAY_HEIGHT / (FONT_HEIGHT + 2)))
#define TFT_LINE_MAX (TFT_COLUMNS + 1)

/* The lines of the dialogue, the oldest is overwritten. */
static char s_lines[TFT_ROWS][TFT_LINE_MAX];
static int s_line_count;
static int s_cursor;
static char s_current[TFT_LINE_MAX];
static bool s_started;

static void draw(void)
{
    int row;
    int column;

    drv_display_clear(0x0000);

    for (row = 0; row < s_line_count; row++) {
        const char *text = s_lines[row];

        drv_display_text(0, row * (FONT_HEIGHT + 2), text, 0xFFFF);
    }

    /* The line that is being written. */
    drv_display_text(0, s_line_count * (FONT_HEIGHT + 2), s_current, 0x07E0);

    (void)column;
    drv_display_flush();
}

/* Copy a line into the buffer, the rest is filled up. */
static void copy_line(char *dst, const char *text)
{
    int i;

    for (i = 0; i < TFT_COLUMNS; i++) {
        dst[i] = (text != NULL && text[i] != '\0') ? text[i] : ' ';
    }
    dst[TFT_COLUMNS] = '\0';
}

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

void tft_console_start(void)
{
    memset(s_lines, 0, sizeof(s_lines));
    s_line_count = 0;
    s_cursor = 0;
    s_current[0] = '\0';

    drv_display_clear(0x0000);
    drv_display_text(0, 0, "MICRO OS", 0x07E0);
    drv_display_flush();

    s_started = true;
}

void tft_console_write(const char *data, k_size_t len)
{
    k_size_t i;

    if (!s_started || data == NULL) {
        return;
    }

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\n') {
            s_current[s_cursor] = '\0';
            push_line(s_current);
            s_cursor = 0;
            s_current[0] = '\0';
            draw();
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
}
