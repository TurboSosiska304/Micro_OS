/*
 * Micro OS — the console of the system.
 */
#include "kernel/console.h"

#include "kernel/port.h"
#include "kernel/task.h"
#include "kernel/wait.h"

/*
 * The sinks that get a copy of everything that is printed. The slot 0
 * belongs to the display (k_console_set_sink), the rest are taken by
 * whoever asks — that way the order of the boot does not matter: the
 * display of the board is set from the shell task, long after the
 * drivers have registered theirs.
 */
static k_console_sink_fn g_sinks[K_CONSOLE_SINK_MAX];

/*
 * The colours of the console. A machine whose screen reads SGR sets this;
 * the serial console of the board and the host process do not, and a line
 * then carries no sequence at all. The flag is written once and read by the
 * log, so it needs no lock of its own.
 */
static bool g_ansi;

void k_console_set_ansi(bool enabled)
{
    g_ansi = enabled;
}

bool k_console_ansi(void)
{
    return g_ansi;
}

void k_console_set_sink(k_console_sink_fn sink)
{
    k_port_enter_critical();
    g_sinks[0] = sink;
    k_port_exit_critical();
}

int k_console_add_sink(k_console_sink_fn sink)
{
    k_size_t i;

    if (sink == NULL) {
        return -1;
    }

    k_port_enter_critical();

    for (i = 1; i < K_CONSOLE_SINK_MAX; i++) {
        if (g_sinks[i] == NULL) {
            g_sinks[i] = sink;
            k_port_exit_critical();
            return 0;
        }
    }

    k_port_exit_critical();

    return -1;
}

void k_console_remove_sink(k_console_sink_fn sink)
{
    k_size_t i;

    if (sink == NULL) {
        return;
    }

    k_port_enter_critical();

    for (i = 0; i < K_CONSOLE_SINK_MAX; i++) {
        if (g_sinks[i] == sink) {
            g_sinks[i] = NULL;
        }
    }

    k_port_exit_critical();
}

void k_console_write(const char *data, k_size_t len)
{
    struct k_terminal *terminal;
    k_console_sink_fn sinks[K_CONSOLE_SINK_MAX];
    k_size_t i;

    if (data == NULL || len == 0) {
        return;
    }

    /*
     * A task with a terminal of its own writes to that terminal and not to
     * the console of the machine: the output of a session of telnet is the
     * output of that session, the way the UART is the output of the console.
     * The two would otherwise be one stream, and two people on the machine
     * at once would read each other's answers.
     */
    terminal = k_terminal_current();

    if (terminal != NULL && terminal->put != NULL) {
        terminal->put(terminal->ctx, data, len);
        return;
    }

    /*
     * The whole write is one critical section: on SMP, two cores writing to
     * the same console interleave their bytes on the wire if each write is
     * serialized only inside k_port_write(). The BKL is recursive per core,
     * so the k_port_write() inside it and a nested klog from the same core
     * are both fine. Sinks are called inside the section as well; the sinks
     * of this tree are display buffers and never sleep.
     */
    k_port_enter_critical();

    k_port_write(data, len);

    for (i = 0; i < K_CONSOLE_SINK_MAX; i++) {
        sinks[i] = g_sinks[i];
    }

    for (i = 0; i < K_CONSOLE_SINK_MAX; i++) {
        if (sinks[i] != NULL) {
            sinks[i](data, len);
        }
    }

    k_port_exit_critical();
}

int k_console_readline(char *buf, k_size_t size)
{
    return k_port_console_readline(buf, size);
}

struct k_terminal *k_terminal_current(void)
{
    struct task *self = task_self();

    return (self != NULL) ? self->terminal : NULL;
}

void k_terminal_set(struct k_terminal *terminal)
{
    struct task *self = task_self();

    if (self != NULL) {
        self->terminal = terminal;
    }
}

int k_console_getchar(void)
{
    struct k_terminal *terminal = k_terminal_current();

    if (terminal != NULL && terminal->get != NULL) {
        return terminal->get(terminal->ctx);
    }

    return k_port_console_getchar();
}

int k_console_poll(void)
{
    return k_port_console_poll();
}

void k_console_terminal_put(const char *data, k_size_t len)
{
    struct k_terminal *terminal = k_terminal_current();

    if (terminal != NULL && terminal->put != NULL) {
        terminal->put(terminal->ctx, data, len);
        return;
    }

    k_console_write(data, len);
}

bool k_console_terminal_echo(void)
{
    struct k_terminal *terminal = k_terminal_current();

    if (terminal == NULL) {
        return true; /* the console of the port: the task echoes the person */
    }

    return (terminal->echo != NULL) ? terminal->echo(terminal->ctx) : false;
}

void k_console_input_notify(void)
{
    k_wait_wake(K_WAIT_CHANNEL_CONSOLE);
}
