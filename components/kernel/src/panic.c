#include "kernel/panic.h"

#include <stdarg.h>
#include <stdio.h>

#include "kernel/log.h"
#include "kernel/port.h"

_Noreturn void kernel_panic(const char *file, int line, const char *fmt, ...) {
    char msg[192];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Panic messages must be visible even if the level was raised. */
    klog_set_level(K_LOG_LEVEL_DEBUG);
    klog(K_LOG_LEVEL_ERROR, "kernel panic: %s", msg);
    klog(K_LOG_LEVEL_ERROR, "at %s:%d", file ? file : "?", line);
    klog(K_LOG_LEVEL_ERROR, "system halted");

    k_port_halt();

    /* k_port_halt() must never return; keep the compiler happy anyway. */
    for (;;) {
    }
}
