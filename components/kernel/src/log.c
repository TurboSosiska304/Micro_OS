#include "kernel/log.h"
#include "kernel/port.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel/config.h"
#include "kernel/console.h"

/* The capture of the log of the boot. */
#define KERNEL_LOG_CAPTURE_SIZE (6 * 1024)

static char g_lines[KERNEL_LOG_LINES][KERNEL_LOG_LINE_MAX];
static k_u32_t g_next;  /* slot to be written next */
static k_u32_t g_count; /* amount of valid entries */
static k_log_level_t g_level = K_LOG_LEVEL_INFO;
static char g_capture[KERNEL_LOG_CAPTURE_SIZE];
static k_size_t g_capture_size;
static bool g_capture_on;
static bool g_capture_mirror;

static k_size_t str_len(const char *s)
{
    k_size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

void klog_init(k_log_level_t level)
{
    g_level = level;
    g_next = 0;
    g_count = 0;
    memset(g_lines, 0, sizeof(g_lines));
}

void klog_set_level(k_log_level_t level)
{
    g_level = level;
}

k_log_level_t klog_get_level(void)
{
    return g_level;
}

const char *klog_level_name(k_log_level_t level)
{
    switch (level) {
    case K_LOG_LEVEL_DEBUG:
        return "debug";
    case K_LOG_LEVEL_INFO:
        return "info";
    case K_LOG_LEVEL_WARN:
        return "warn";
    case K_LOG_LEVEL_ERROR:
        return "error";
    case K_LOG_LEVEL_OFF:
        return "off";
    default:
        return "?";
    }
}

/*
 * The SGR sequence of a tone on a screen with colours, or NULL for a line
 * that is read as it is. Amber is the yellow of the palette (6, the "brown"
 * of a CGA), the colour of an ASCII terminal that warns; red is its red and
 * green the green of a success.
 */
static const char *tone_sgr(k_log_level_t level, k_log_tone_t tone)
{
    if (tone == K_LOG_TONE_OK) {
        return "32";
    }

    switch (level) {
    case K_LOG_LEVEL_WARN:
        return "33";
    case K_LOG_LEVEL_ERROR:
        return "31";
    default:
        return NULL;
    }
}

static void klog_emit(k_log_level_t level, k_log_tone_t tone, const char *fmt, va_list ap)
{
    char buf[KERNEL_LOG_LINE_MAX];
    int written;

    if (level < g_level || level == K_LOG_LEVEL_OFF || level > K_LOG_LEVEL_OFF) {
        return;
    }

    written = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (written < 0) {
        return;
    }

    /* Truncated output is still useful, but make sure we are NUL terminated. */
    buf[KERNEL_LOG_LINE_MAX - 1] = '\0';

    bool capture;

    /*
     * The ring is written by every task that logs, and the kernel logs from
     * inside its own critical sections - so the sections here are recursive
     * and the kernel dies not wake up with a half written line.
     */
    k_port_enter_critical();

    /* Retain the message for /proc/kmsg. */
    memcpy(g_lines[g_next], buf, KERNEL_LOG_LINE_MAX);
    g_next = (g_next + 1) % KERNEL_LOG_LINES;
    if (g_count < KERNEL_LOG_LINES) {
        g_count++;
    }

    capture = g_capture_on;

    if (capture) {
        /* The log of the boot is collected, not printed. */
        k_size_t len = str_len(buf);

        if (g_capture_size + len + 2 < KERNEL_LOG_CAPTURE_SIZE) {
            memcpy(g_capture + g_capture_size, buf, len);
            g_capture_size += len;
            g_capture[g_capture_size++] = '\n';
            g_capture[g_capture_size] = '\0';
        }
    }

    k_port_exit_critical();

    if (capture && !g_capture_mirror) {
        return;
    }

    /*
     * One write: two tasks that log at the same moment used to leave half
     * of one line and half of another on the same row. The ring and the
     * capture of the boot keep the text without a sequence; the colours are
     * a mark of the screen and only a console that reads them gets them.
     */
    {
        k_size_t len = str_len(buf);
        const char *sgr = k_console_ansi() ? tone_sgr(level, tone) : NULL;

        if (len >= KERNEL_LOG_LINE_MAX - 1) {
            len = KERNEL_LOG_LINE_MAX - 2;
            buf[len] = '\0';
        }

        if (sgr == NULL) {
            buf[len] = '\n';
            buf[len + 1] = '\0';
            k_console_write(buf, len + 1);
        } else {
            /* The reset comes before the end of the line, so a terminal that
             * paints the end of a row in a colour does not paint the next
             * one; the serial copy drops the two sequences whole. */
            char line[KERNEL_LOG_LINE_MAX + 12];
            int total = snprintf(line, sizeof(line), "\033[%sm%s\033[0m\n", sgr, buf);

            if (total > 0) {
                k_console_write(line, (k_size_t)total < sizeof(line) ? (k_size_t)total
                                                                     : sizeof(line) - 1);
            }
        }
    }
}

void klog(k_log_level_t level, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    klog_emit(level, K_LOG_TONE_PLAIN, fmt, ap);
    va_end(ap);
}

void klog_tone(k_log_tone_t tone, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    klog_emit(K_LOG_LEVEL_INFO, tone, fmt, ap);
    va_end(ap);
}

static void capture_start(bool mirror)
{
    k_port_enter_critical();
    g_capture_size = 0;
    g_capture[0] = '\0';
    g_capture_on = true;
    g_capture_mirror = mirror;
    k_port_exit_critical();
}

void klog_capture_start(void)
{
    capture_start(false);
}

/* A port with a console from the first instruction can keep the boot capture
 * and still print it. The board keeps the original collect-then-write mode. */
void klog_capture_start_mirroring(void)
{
    capture_start(true);
}

void klog_capture_stop(void)
{
    k_port_enter_critical();
    g_capture_on = false;
    g_capture_mirror = false;
    k_port_exit_critical();
}

const char *klog_captured_text(void)
{
    return g_capture;
}

k_size_t klog_captured_size(void)
{
    return g_capture_size;
}

k_size_t klog_line_count(void)
{
    return (k_size_t)g_count;
}

void klog_clear(void)
{
    g_next = 0;
    g_count = 0;
    memset(g_lines, 0, sizeof(g_lines));
}

k_size_t klog_dump(char *out, k_size_t out_size)
{
    k_u32_t i;
    k_size_t used = 0;
    k_size_t needed = 0;
    k_u32_t fit = 0;

    if (out == NULL || out_size == 0) {
        return 0;
    }

    out[0] = '\0';

    /*
     * The output buffer can be smaller than the ring buffer. In that case the
     * newest messages are kept and the oldest ones are dropped, so that
     * "dmesg" always shows the current state of the system.
     */
    for (i = 0; i < g_count; i++) {
        k_u32_t idx = (g_next + KERNEL_LOG_LINES - g_count + i) % KERNEL_LOG_LINES;
        k_size_t len = str_len(g_lines[idx]);

        if (needed + len + 1 > out_size - 1) {
            break;
        }
        needed += len + 1;
        fit++;
    }

    for (i = g_count - fit; i < g_count; i++) {
        k_u32_t idx = (g_next + KERNEL_LOG_LINES - g_count + i) % KERNEL_LOG_LINES;
        k_size_t len = str_len(g_lines[idx]);

        memcpy(out + used, g_lines[idx], len);
        used += len;
        out[used++] = '\n';
    }

    out[used] = '\0';
    return used;
}
