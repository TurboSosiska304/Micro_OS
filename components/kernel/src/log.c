#include "kernel/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel/config.h"
#include "kernel/port.h"

static char g_lines[KERNEL_LOG_LINES][KERNEL_LOG_LINE_MAX];
static k_u32_t g_next;  /* slot to be written next */
static k_u32_t g_count; /* amount of valid entries */
static k_log_level_t g_level = K_LOG_LEVEL_INFO;

static k_size_t str_len(const char *s) {
    k_size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

void klog_init(k_log_level_t level) {
    g_level = level;
    g_next = 0;
    g_count = 0;
    memset(g_lines, 0, sizeof(g_lines));
}

void klog_set_level(k_log_level_t level) {
    g_level = level;
}

k_log_level_t klog_get_level(void) {
    return g_level;
}

const char *klog_level_name(k_log_level_t level) {
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

void klog(k_log_level_t level, const char *fmt, ...) {
    char buf[KERNEL_LOG_LINE_MAX];
    va_list ap;
    int written;

    if (level < g_level || level == K_LOG_LEVEL_OFF || level > K_LOG_LEVEL_OFF) {
        return;
    }

    va_start(ap, fmt);
    written = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (written < 0) {
        return;
    }

    /* Truncated output is still useful, but make sure we are NUL terminated. */
    buf[KERNEL_LOG_LINE_MAX - 1] = '\0';

    /* Retain the message for /proc/kmsg. */
    memcpy(g_lines[g_next], buf, KERNEL_LOG_LINE_MAX);
    g_next = (g_next + 1) % KERNEL_LOG_LINES;
    if (g_count < KERNEL_LOG_LINES) {
        g_count++;
    }

    k_port_write(buf, str_len(buf));
    k_port_write("\n", 1);
}

k_size_t klog_line_count(void) {
    return (k_size_t)g_count;
}

void klog_clear(void) {
    g_next = 0;
    g_count = 0;
    memset(g_lines, 0, sizeof(g_lines));
}

k_size_t klog_dump(char *out, k_size_t out_size) {
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
