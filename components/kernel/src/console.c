/*
 * Micro OS — the console of the system.
 */
#include "kernel/console.h"

#include "kernel/port.h"

/*
 * The sinks that get a copy of everything that is printed. The slot 0
 * belongs to the display (k_console_set_sink), the rest are taken by
 * whoever asks — that way the order of the boot does not matter: the
 * display of the board is set from the shell task, long after the
 * drivers have registered theirs.
 */
static k_console_sink_fn g_sinks[K_CONSOLE_SINK_MAX];

void k_console_set_sink(k_console_sink_fn sink)
{
    g_sinks[0] = sink;
}

int k_console_add_sink(k_console_sink_fn sink)
{
    k_size_t i;

    if (sink == NULL) {
        return -1;
    }

    for (i = 1; i < K_CONSOLE_SINK_MAX; i++) {
        if (g_sinks[i] == NULL) {
            g_sinks[i] = sink;
            return 0;
        }
    }

    return -1;
}

void k_console_write(const char *data, k_size_t len)
{
    k_size_t i;

    if (data == NULL || len == 0) {
        return;
    }

    k_port_write(data, len);

    for (i = 0; i < K_CONSOLE_SINK_MAX; i++) {
        if (g_sinks[i] != NULL) {
            g_sinks[i](data, len);
        }
    }
}

int k_console_readline(char *buf, k_size_t size)
{
    return k_port_console_readline(buf, size);
}

int k_console_getchar(void)
{
    return k_port_console_getchar();
}

int k_console_poll(void)
{
    return k_port_console_poll();
}
