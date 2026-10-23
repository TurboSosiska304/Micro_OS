/*
 * Micro OS — `mfetch`, the logo and the report of the system.
 *
 * The analog of fastfetch: the logo of the system on the left and a short
 * list of what the board is on the right. The logo is the size "L" of
 * logos.md, the file in the root of the repository.
 *
 * The logo belongs to the serial console. The screen of the board draws
 * only the characters of its 5x7 font, which stops at 0x5F, and the box
 * drawing characters of the logo would be rows of '?' there. The report
 * itself is plain ASCII: it is the same and it is readable everywhere.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"

/*
 * The two colors of the logo: green like the name that the screen of the
 * board shows at the start, white for the rest. `mfetch color` turns them
 * on, the plain `mfetch` keeps every byte of the output printable.
 */
#define MFETCH_COLOR_1 "\033[32m"
#define MFETCH_COLOR_2 "\033[37m"
#define MFETCH_COLOR_OFF "\033[0m"

/*
 * The logo: `$1` and `$2` mark the color of a piece, exactly as in
 * logos.md. They are not characters of the drawing and they disappear
 * when there is no color.
 */
static const char *const g_logo[] = {
    "$1███╗   ███╗$2 ██████╗ ███████╗", "$1████╗ ████║$2██╔═══██╗██╔════╝",
    "$1██╔████╔██║$2██║   ██║███████╗", "$1██║╚██╔╝██║$2██║   ██║╚════██║",
    "$1██║ ╚═╝ ██║$2╚██████╔╝███████║", "$1╚═╝     ╚═╝$2 ╚═════╝ ╚══════╝",
};

#define MFETCH_LOGO_LINES (sizeof(g_logo) / sizeof(g_logo[0]))

/* The columns of the longest line of the logo, and the two of the gap. */
#define MFETCH_LOGO_COLS 28
#define MFETCH_INFO_COLS (MFETCH_LOGO_COLS + 2)

/* The width of the column of the names of the report. */
#define MFETCH_KEY_COLS 12

/*
 * One line of the output. It has to stay under the buffer of
 * kshell_printf, which holds 192 bytes.
 */
#define MFETCH_LINE_MAX 176

static void append_char(char *line, k_size_t *len, char c)
{
    if (*len + 2 < MFETCH_LINE_MAX) {
        line[(*len)++] = c;
    }
}

static void append(char *line, k_size_t *len, const char *text)
{
    while (*text != '\0') {
        append_char(line, len, *text++);
    }
}

/*
 * The logo of the row, padded to the column where the report begins. The
 * rows that are below the logo get the empty place instead of it, so that
 * the names of the report stay in one column.
 */
static void append_logo(char *line, k_size_t *len, k_size_t row, bool color)
{
    const char *text = (row < MFETCH_LOGO_LINES) ? g_logo[row] : NULL;
    k_size_t cols = 0;

    while (text != NULL && *text != '\0') {
        if (text[0] == '$' && (text[1] == '1' || text[1] == '2')) {
            if (color) {
                append(line, len, (text[1] == '1') ? MFETCH_COLOR_1 : MFETCH_COLOR_2);
            }
            text += 2;
            continue;
        }

        /*
         * One character of the drawing: the continuation bytes of a UTF-8
         * sequence are not columns of their own.
         */
        if (((unsigned char)*text & 0xC0u) != 0x80u) {
            cols++;
        }
        append_char(line, len, *text);
        text++;
    }

    if (color && row < MFETCH_LOGO_LINES) {
        append(line, len, MFETCH_COLOR_OFF);
    }

    while (cols < MFETCH_INFO_COLS) {
        append_char(line, len, ' ');
        cols++;
    }
}

/* One row of the report: the logo of the row, the name and the value. */
static void print_row(k_size_t row, bool color, const char *key, const char *value)
{
    char line[MFETCH_LINE_MAX];
    char label[MFETCH_KEY_COLS + 1];
    k_size_t len = 0;

    append_logo(line, &len, row, color);

    (void)snprintf(label, sizeof(label), "%-*s", MFETCH_KEY_COLS, key);
    append(line, &len, label);
    append(line, &len, " ");
    append(line, &len, value);
    append(line, &len, "\n");
    line[len] = '\0';

    kshell_printf("%s", line);
}

static int cmd_mfetch(int argc, char **argv)
{
    const struct board_info *info = board_get_info();
    char value[48];
    bool color = false;
    k_size_t row = 0;

    if (argc > 1) {
        if (strcmp(argv[1], "color") == 0 || strcmp(argv[1], "--color") == 0) {
            color = true;
        } else {
            kshell_printf("usage: mfetch [color]\n");
            return KSHELL_USAGE;
        }
    }

    (void)snprintf(value, sizeof(value), "%s %s", kernel_name(), kernel_version());
    print_row(row++, color, "OS", value);

    print_row(row++, color, "Board", info->desc);
    print_row(row++, color, "SoC", info->soc);
    print_row(row++, color, "Arch", kernel_arch());
    print_row(row++, color, "Display", info->lcd);

    (void)snprintf(value, sizeof(value), "heap %u/%u KB, psram %u KB free",
                   (unsigned)(k_port_heap_free() / 1024), (unsigned)(k_port_heap_total() / 1024),
                   (unsigned)(k_port_psram_free() / 1024));
    print_row(row++, color, "Memory", value);

    (void)snprintf(value, sizeof(value), "%llu s, %u task(s)",
                   (unsigned long long)(k_sched_uptime_ms() / 1000), (unsigned)task_count());
    print_row(row++, color, "Uptime", value);

    (void)snprintf(value, sizeof(value), "%u command(s)", (unsigned)kshell_command_count());
    print_row(row++, color, "Shell", value);

    return KSHELL_OK;
}

int kshell_register_mfetch(void)
{
    return kshell_register("mfetch", "the system report with the logo", cmd_mfetch);
}
