/*
 * Micro OS — the console of the system.
 */
#include "kernel/console.h"

#include "kernel/port.h"

static k_console_sink_fn g_sink;

void k_console_set_sink(k_console_sink_fn sink)
{
    g_sink = sink;
}

void k_console_write(const char *data, k_size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    k_port_write(data, len);

    if (g_sink != NULL) {
        g_sink(data, len);
    }
}
