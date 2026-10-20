#include "kshell/kshell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel/kmod.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/version.h"

struct command {
    const char *name;
    const char *help;
    kshell_cmd_fn fn;
};

static struct command g_commands[KSHELL_MAX_COMMANDS];
static k_size_t g_command_count;

static void copy_str(char *dst, k_size_t size, const char *src)
{
    if (size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
}

int kshell_register(const char *name, const char *help, kshell_cmd_fn fn)
{
    k_size_t i;

    if (name == NULL || fn == NULL) {
        return KSHELL_ERROR;
    }

    for (i = 0; i < g_command_count; i++) {
        if (strcmp(g_commands[i].name, name) == 0) {
            g_commands[i].help = help;
            g_commands[i].fn = fn;
            return KSHELL_OK;
        }
    }

    if (g_command_count >= KSHELL_MAX_COMMANDS) {
        return KSHELL_ERROR;
    }

    g_commands[g_command_count].name = name;
    g_commands[g_command_count].help = help;
    g_commands[g_command_count].fn = fn;
    g_command_count++;

    return KSHELL_OK;
}

int kshell_unregister(const char *name)
{
    k_size_t i;

    if (name == NULL) {
        return KSHELL_ERROR;
    }

    for (i = 0; i < g_command_count; i++) {
        if (strcmp(g_commands[i].name, name) == 0) {
            g_commands[i] = g_commands[g_command_count - 1];
            g_command_count--;
            return KSHELL_OK;
        }
    }

    return KSHELL_ERROR;
}

k_size_t kshell_command_count(void)
{
    return g_command_count;
}

void kshell_printf(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    int written;

    va_start(ap, fmt);
    written = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (written < 0) {
        return;
    }

    buf[sizeof(buf) - 1] = '\0';
    k_port_write(buf, strlen(buf));
}

/* Split a line into arguments. Returns the number of arguments. */
static int split_args(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *cursor = line;

    while (*cursor != '\0' && argc < max_args) {
        while (*cursor == ' ' || *cursor == '\t') {
            *cursor++ = '\0';
        }

        if (*cursor == '\0') {
            break;
        }

        argv[argc++] = cursor;

        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }
    }

    return argc;
}

static struct command *find_command(const char *name)
{
    k_size_t i;

    for (i = 0; i < g_command_count; i++) {
        if (strcmp(g_commands[i].name, name) == 0) {
            return &g_commands[i];
        }
    }

    return NULL;
}

int kshell_execute(const char *line)
{
    char buffer[KSHELL_LINE_MAX];
    char *argv[KSHELL_MAX_ARGS];
    int argc;
    struct command *cmd;

    if (line == NULL) {
        return KSHELL_ERROR;
    }

    copy_str(buffer, sizeof(buffer), line);

    /* Strip the trailing newline. */
    {
        k_size_t len = strlen(buffer);
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }
    }

    argc = split_args(buffer, argv, KSHELL_MAX_ARGS);
    if (argc == 0) {
        return KSHELL_OK;
    }

    cmd = find_command(argv[0]);
    if (cmd == NULL) {
        kshell_printf("%s: command not found\n", argv[0]);
        return KSHELL_ERROR;
    }

    return cmd->fn(argc, argv);
}

/* --- the built in commands --- */

static int cmd_help(int argc, char **argv)
{
    k_size_t i;

    (void)argc;
    (void)argv;

    kshell_printf("commands:\n");
    for (i = 0; i < g_command_count; i++) {
        kshell_printf("  %-10s %s\n", g_commands[i].name,
                      g_commands[i].help != NULL ? g_commands[i].help : "");
    }

    return KSHELL_OK;
}

static int cmd_uname(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kshell_printf("%s %s %s (%s)\n", kernel_name(), kernel_version(), k_port_arch(),
                  kernel_build_id());

    return KSHELL_OK;
}

static int cmd_uptime(int argc, char **argv)
{
    k_u64_t ms = k_sched_uptime_ms();

    (void)argc;
    (void)argv;

    kshell_printf("up %llu s, %u tick(s), %u task(s)\n", (unsigned long long)(ms / 1000),
                  (unsigned)k_sched_ticks(), (unsigned)task_count());

    return KSHELL_OK;
}

static int cmd_ps(int argc, char **argv)
{
    struct task *self = task_self();
    k_size_t i;

    (void)argc;
    (void)argv;

    kshell_printf("  pid  prio  state     name\n");
    kshell_printf("  ---  ----  --------  --------\n");

    for (i = 0; i < TASK_MAX_TASKS; i++) {
        struct task *t = task_get_by_index(i);
        const char *state;

        if (t == NULL) {
            continue;
        }

        state = (t == self) ? "running" : task_state_name(t->state);
        kshell_printf("  %3u  %4u  %-8s  %s\n", (unsigned)t->pid, (unsigned)t->prio, state,
                      t->name);
    }

    return KSHELL_OK;
}

static int cmd_echo(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        kshell_printf("%s%s", argv[i], (i + 1 < argc) ? " " : "\n");
    }

    if (argc == 1) {
        kshell_printf("\n");
    }

    return KSHELL_OK;
}

static int cmd_sleep(int argc, char **argv)
{
    unsigned long ms;

    if (argc < 2) {
        kshell_printf("usage: sleep <milliseconds>\n");
        return KSHELL_USAGE;
    }

    ms = strtoul(argv[1], NULL, 10);
    k_sched_sleep_ms((k_u32_t)ms);
    kshell_printf("slept %lu ms\n", ms);

    return KSHELL_OK;
}

static int cmd_dmesg(int argc, char **argv)
{
    static char buf[2048];

    (void)argc;
    (void)argv;

    klog_dump(buf, sizeof(buf));
    kshell_printf("%s", buf);

    return KSHELL_OK;
}

static int cmd_free(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kshell_printf("heap: %u KB free of %u KB, psram: %u KB free\n",
                  (unsigned)(k_port_heap_free() / 1024), (unsigned)(k_port_heap_total() / 1024),
                  (unsigned)(k_port_psram_free() / 1024));

    return KSHELL_OK;
}

static int cmd_modules(int argc, char **argv)
{
    int problems;

    (void)argc;
    (void)argv;

    problems = kmod_doctor();
    kmod_report();

    return problems == 0 ? KSHELL_OK : KSHELL_ERROR;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kshell_printf("rebooting...\n");
    k_port_reset();

    return KSHELL_OK;
}

static int cmd_clear(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* ANSI clear screen and home the cursor. */
    kshell_printf("\033[2J\033[H");

    return KSHELL_OK;
}

static int kshell_register_builtins(void)
{
    int rc = 0;

    rc |= kshell_register("help", "list the available commands", cmd_help);
    rc |= kshell_register("uname", "kernel name and version", cmd_uname);
    rc |= kshell_register("uptime", "time since the boot", cmd_uptime);
    rc |= kshell_register("ps", "list the tasks", cmd_ps);
    rc |= kshell_register("echo", "print the arguments", cmd_echo);
    rc |= kshell_register("sleep", "sleep <milliseconds>", cmd_sleep);
    rc |= kshell_register("dmesg", "the kernel log", cmd_dmesg);
    rc |= kshell_register("free", "the memory usage", cmd_free);
    rc |= kshell_register("clear", "clear the screen", cmd_clear);
    rc |= kshell_register("modules", "check the hardware modules", cmd_modules);
    rc |= kshell_register("reboot", "restart the board", cmd_reboot);

    return rc;
}

void kshell_run(void)
{
    char line[KSHELL_LINE_MAX];

    kshell_register_builtins();

    kshell_printf("%s shell ready. Type 'help' for the list of commands.\n", kernel_name());

    for (;;) {
        kshell_printf("microos> ");

        if (k_port_console_readline(line, sizeof(line)) < 0) {
            /* No input available: do not spin. */
            kshell_printf("\n");
            k_sched_sleep_ms(20);
            continue;
        }

        kshell_execute(line);
    }
}
