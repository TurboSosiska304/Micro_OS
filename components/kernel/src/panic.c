#include "kernel/panic.h"

#include <stdarg.h>
#include <stdio.h>

#include "kernel/log.h"
#include "kernel/port.h"

static k_size_t str_len(const char *text)
{
    k_size_t len = 0;

    while (text[len] != '\0') {
        len++;
    }

    return len;
}

[[noreturn]] void kernel_panic(const char *file, int line, const char *fmt, ...)
{
    char msg[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /*
     * The screen first: a program that owns the framebuffer (ADR 0028) has
     * to lose it, because an interface that hides a panic is worse than a
     * panic. The call is the port's own and does nothing on a machine whose
     * console nobody can take.
     */
    k_port_console_panic();

    /*
     * The console next, without the log: the panic may have happened with
     * the lock of the console in the hands of the task that is dying, and
     * a diagnostic that waits for a mutex is no diagnostic at all.
     */
    k_port_write("kernel panic: ", 14);
    k_port_write(msg, str_len(msg));
    k_port_write("\n", 1);

    /* Then the log, if it can be had, for the ring and the sinks. */
    klog_set_level(K_LOG_LEVEL_DEBUG);
    klog(K_LOG_LEVEL_ERROR, "kernel panic: %s", msg);
    klog(K_LOG_LEVEL_ERROR, "at %s:%d", file ? file : "?", line);

    k_port_halt();

    /* k_port_halt() must never return; keep the compiler happy anyway. */
    for (;;) {
    }
}
