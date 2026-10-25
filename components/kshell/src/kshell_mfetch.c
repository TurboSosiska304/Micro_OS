/*
 * Micro OS — `mfetch`, the logo and the report of the system.
 *
 * The analog of fastfetch: the logo of the system on the left and a
 * short list of what the board is on the right — the version and the
 * build, the hardware, the memory with the blocks that live in it, the
 * volume of the flash, the uptime with the tasks, the modules of the
 * doctor and the shell.
 *
 * One drawing has to serve two places: the serial console and the
 * screen of the board. The size "L" of logos.md (the file in the root
 * of the repository) is drawn in box drawing characters, and the 5x7
 * font of the screen stops at 0x5F — on the display every byte of it
 * would be a row of '?'. So the logo is the letter M in the characters
 * of that font: it is the same everywhere and it is readable
 * everywhere, and it leaves the room beside it for the report.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "board/board.h"
#include "kernel/kmm.h"
#include "kernel/kmod.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"
#include "kernel/vfs.h"

/*
 * The two colors of the logo: green like the name that the screen of the
 * board shows at the start, white for the rest. `mfetch color` turns them
 * on, the plain `mfetch` keeps every byte of the output printable.
 */
#define MFETCH_COLOR_1 "\033[32m"
#define MFETCH_COLOR_2 "\033[37m"
#define MFETCH_COLOR_OFF "\033[0m"

/*
 * The logo: the letter M, in the characters of the font of the board.
 *
 * `$1` and `$2` mark the color of a piece, exactly as in logos.md.
 * They are not characters of the drawing and they disappear when there
 * is no color. One line of the logo per line of the source: clang-format,
 * which counts columns, would otherwise pack two lines into one.
 */
// clang-format off
static const char *const g_logo[] = {
    "$1M   M",
    "$1MM MM",
    "$1M M M",
    "$1M   M",
    "$1M   M",
};
// clang-format on

#define MFETCH_LOGO_LINES (sizeof(g_logo) / sizeof(g_logo[0]))

/* The columns of the letter of the logo, and the two of the gap. */
#define MFETCH_LOGO_COLS 5
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
    print_row(row++, color, "Build", kernel_build_id());

    print_row(row++, color, "Board", info->desc);
    print_row(row++, color, "SoC", info->soc);
    print_row(row++, color, "Arch", kernel_arch());
    print_row(row++, color, "Display", info->lcd);

    (void)snprintf(value, sizeof(value), "%u/%u KB free", (unsigned)(k_port_heap_free() / 1024),
                   (unsigned)(k_port_heap_total() / 1024));
    print_row(row++, color, "Heap", value);

    (void)snprintf(value, sizeof(value), "%u KB free", (unsigned)(k_port_psram_free() / 1024));
    print_row(row++, color, "Psram", value);

    {
        struct kmm_stats stats;

        kmm_get_stats(&stats);
        (void)snprintf(value, sizeof(value), "%u block(s), %u KB", (unsigned)stats.allocations,
                       (unsigned)(stats.total_bytes / 1024));
        print_row(row++, color, "Allocs", value);
    }

    {
        struct vfs_statfs stat;

        if (vfs_statfs("/", &stat) == 0) {
            k_u64_t free_kb = ((k_u64_t)stat.free_blocks * (k_u64_t)stat.block_size) / 1024u;
            k_u64_t total_kb = ((k_u64_t)stat.total_blocks * (k_u64_t)stat.block_size) / 1024u;

            (void)snprintf(value, sizeof(value), "%u of %u KB", (unsigned)free_kb,
                           (unsigned)total_kb);
        } else {
            (void)snprintf(value, sizeof(value), "not mounted");
        }
        print_row(row++, color, "Volume", value);
    }

    (void)snprintf(value, sizeof(value), "%llu s, %u task(s)",
                   (unsigned long long)(k_sched_uptime_ms() / 1000), (unsigned)task_count());
    print_row(row++, color, "Uptime", value);

    (void)snprintf(value, sizeof(value), "%u, %d problem(s)", (unsigned)kmod_count(),
                   kmod_problems());
    print_row(row++, color, "Modules", value);

    (void)snprintf(value, sizeof(value), "%u command(s)", (unsigned)kshell_command_count());
    print_row(row++, color, "Shell", value);

    return KSHELL_OK;
}

int kshell_register_mfetch(void)
{
    return kshell_register("mfetch", "the system report with the logo", cmd_mfetch);
}
