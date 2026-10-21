/*
 * Micro OS — the console on the display of the board.
 *
 * The output of the shell is mirrored to the screen: the last lines of the
 * dialogue are shown, the text is written with the small font of the board.
 */
#include "graphics/tft_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drivers/display.h"
#include "graphics/font.h"
#include "drivers/display.h"
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

static void draw(void);

void tft_console_redraw(void)
{
    draw();
}

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

int tft_console_register(void)
{
    return kshell_register("display", "the offset of the image on the screen", cmd_display);
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
