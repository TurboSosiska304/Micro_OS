/*
 * Micro OS — `mfetch`, the logo and the report of the system.
 *
 * The analog of fastfetch: the logo of the system on the left and a
 * short list of what the machine is on the right — the version, the
 * machine, the cores that the kernel runs, the memory and the disk with
 * a bar, the uptime with the tasks and the modules of the doctor.
 *
 * The report is chosen for a person at the keyboard, not for a debugger:
 * a build id, a count of allocations and the number of commands of the
 * shell say nothing about the machine, and they are gone. What is left is
 * what changes between two boots and between two machines.
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

#include "kernel/board.h"
#include "kernel/config.h"
#include "kernel/cpu.h"
#include "kernel/kmod.h"
#include "kernel/pagecache.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"
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

/* The cells of the bar of the memory and of the disk. */
#define MFETCH_BAR_COLS 10

/* The unit of a whole: a machine of the board has kilobytes and a machine of
 * x86-64 has mebibytes, and two numbers of one row must share the unit. */
static bool size_in_mib(k_u64_t bytes)
{
    return bytes >= (1024u * 1024u);
}

/* A size in bytes as a person reads it: mebibytes with one decimal, or the
 * whole kilobytes when the unit of the row is the kilobyte. */
static void format_size(char *out, k_size_t out_size, k_u64_t bytes, bool mib)
{
    if (mib) {
        k_u64_t whole = bytes / (1024u * 1024u);
        k_u64_t tenth = ((bytes % (1024u * 1024u)) * 10u) / (1024u * 1024u);

        (void)snprintf(out, out_size, "%llu.%llu MiB", (unsigned long long)whole,
                       (unsigned long long)tenth);
    } else {
        (void)snprintf(out, out_size, "%llu KiB", (unsigned long long)(bytes / 1024u));
    }
}

/*
 * The bar of a whole and the part of it in use, with the percentage. A whole
 * of zero is a machine that has not told us its memory yet: the bar is empty
 * and the number says zero, which is honest, instead of dividing by it.
 */
static void format_bar(char *out, k_size_t out_size, k_u64_t used, k_u64_t total)
{
    char bar[MFETCH_BAR_COLS + 1];
    unsigned filled = 0;
    unsigned percent = 0;
    k_size_t i;

    if (total != 0u) {
        filled = (unsigned)((used * MFETCH_BAR_COLS) / total);
        percent = (unsigned)((used * 100u) / total);
    }
    if (filled > MFETCH_BAR_COLS) {
        filled = MFETCH_BAR_COLS;
    }
    for (i = 0; i < MFETCH_BAR_COLS; i++) {
        bar[i] = (i < filled) ? '#' : '-';
    }
    bar[MFETCH_BAR_COLS] = '\0';

    (void)snprintf(out, out_size, "[%s] %u%%", bar, percent);
}

/*
 * The time since the boot in the two or three units that matter: a machine
 * that has run for a day says the day, one that has just started says the
 * seconds. The report is read at a glance, so it carries no zero in front.
 */
static void format_uptime(char *out, k_size_t out_size, k_u64_t ms)
{
    k_u64_t total = ms / 1000u;
    k_u64_t days = total / 86400u;
    k_u64_t hours = (total % 86400u) / 3600u;
    k_u64_t minutes = (total % 3600u) / 60u;
    k_u64_t seconds = total % 60u;

    if (days != 0u) {
        (void)snprintf(out, out_size, "%llud %lluh %llum", (unsigned long long)days,
                       (unsigned long long)hours, (unsigned long long)minutes);
    } else if (hours != 0u) {
        (void)snprintf(out, out_size, "%lluh %llum %llus", (unsigned long long)hours,
                       (unsigned long long)minutes, (unsigned long long)seconds);
    } else if (minutes != 0u) {
        (void)snprintf(out, out_size, "%llum %llus", (unsigned long long)minutes,
                       (unsigned long long)seconds);
    } else {
        (void)snprintf(out, out_size, "%llus", (unsigned long long)seconds);
    }
}

static int cmd_mfetch(int argc, char **argv)
{
    const struct kernel_board *info = k_board_get();
    char value[128];
    char used_text[24];
    char total_text[24];
    char bar[24];
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

    /*
     * The machine in one line. A board names itself, and the port that is not
     * a board names its architecture and its platform: "x86_64, x86-64" would
     * be a stutter, so the description of the board wins when there is one.
     */
    if (info != NULL && info->desc != NULL) {
        (void)snprintf(value, sizeof(value), "%s", info->desc);
    } else {
        (void)snprintf(value, sizeof(value), "%s, %s", kernel_arch(), k_sysinfo_platform_name());
    }
    print_row(row++, color, "Machine", value);

    /*
     * The cores: what the machine has and what the kernel runs. The two
     * numbers differ on a build without KERNEL_SMP, and a person who asks
     * the machine what it is wants to see exactly that.
     */
    (void)snprintf(value, sizeof(value), "%u core(s), %u in use", (unsigned)k_cpu_count(),
                   (unsigned)k_sched_cpu_count());
    print_row(row++, color, "CPU", value);

    {
        k_u64_t total = (k_u64_t)k_sysinfo_heap_total();
        k_u64_t used =
            (total > (k_u64_t)k_sysinfo_heap_free()) ? total - (k_u64_t)k_sysinfo_heap_free() : 0u;

        bool mib = size_in_mib(total);

        format_bar(bar, sizeof(bar), used, total);
        format_size(used_text, sizeof(used_text), used, mib);
        format_size(total_text, sizeof(total_text), total, mib);

#if KERNEL_PSRAM
        {
            k_u64_t psram_total = (k_u64_t)k_sysinfo_psram_total();
            k_u64_t psram_free = (k_u64_t)k_sysinfo_psram_free();
            char psram_used_text[24];
            char psram_total_text[24];

            format_size(psram_used_text, sizeof(psram_used_text),
                        (psram_total > psram_free) ? psram_total - psram_free : 0u,
                        size_in_mib(psram_total));
            format_size(psram_total_text, sizeof(psram_total_text), psram_total,
                        size_in_mib(psram_total));
            (void)snprintf(value, sizeof(value), "%s %s/%s, psram %s/%s", bar, used_text,
                           total_text, psram_used_text, psram_total_text);
        }
#else
        (void)snprintf(value, sizeof(value), "%s %s/%s", bar, used_text, total_text);
#endif
        print_row(row++, color, "Memory", value);
    }

    {
        struct vfs_statfs stat;

        if (vfs_statfs("/", &stat) == 0 && stat.total_blocks != 0u) {
            k_u64_t block = (k_u64_t)stat.block_size;
            k_u64_t total = (k_u64_t)stat.total_blocks * block;
            k_u64_t used = total - (k_u64_t)stat.free_blocks * block;

            bool mib = size_in_mib(total);

            format_bar(bar, sizeof(bar), used, total);
            format_size(used_text, sizeof(used_text), used, mib);
            format_size(total_text, sizeof(total_text), total, mib);
            (void)snprintf(value, sizeof(value), "%s %s/%s", bar, used_text, total_text);
        } else {
            (void)snprintf(value, sizeof(value), "not mounted");
        }
        print_row(row++, color, "Disk", value);
    }

    {
        char up[32];

        format_uptime(up, sizeof(up), k_sched_uptime_ms());
        (void)snprintf(value, sizeof(value), "%s, %u task(s)", up, (unsigned)task_count());
        print_row(row++, color, "Uptime", value);
    }

    (void)snprintf(value, sizeof(value), "%u, %d problem(s)", (unsigned)kmod_count(),
                   kmod_problems());
    print_row(row++, color, "Modules", value);

    {
        struct pagecache_stats pc;

        pagecache_get_stats(&pc);
        (void)snprintf(value, sizeof(value),
                       "%llu/%llu pages, %llu dirty, %llu hits, %llu misses, %llu evicted",
                       (unsigned long long)pc.pages, (unsigned long long)pc.capacity,
                       (unsigned long long)pc.dirty, (unsigned long long)pc.hits,
                       (unsigned long long)pc.misses, (unsigned long long)pc.evictions);
        print_row(row++, color, "Page cache", value);
    }

    return KSHELL_OK;
}

int kshell_register_mfetch(void)
{
    static const struct kshell_help help = {
        .summary = "the system report with the logo",
        .usage = "mfetch [color]",
        .detail = "The logo beside a short report: the version, the machine, the cores the\n"
                  "kernel runs, the memory and the disk with a bar, the uptime with the\n"
                  "tasks and the modules of the doctor.\n"
                  "`color` brings the escape sequences of the terminal; the plain report\n"
                  "stays printable on the screen of the board.\n",
    };

    return kshell_register_help("mfetch", &help, cmd_mfetch);
}
