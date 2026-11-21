#include "kshell/kshell.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libc/stdlib.h"
#include "libc/unistd.h"
#include "kernel/config.h"
#include "kernel/kmod.h"
#include "kernel/ksys.h"
#include "kernel/console.h"
#include "kernel/log.h"
#include "kernel/power.h"
#include "kernel/sched.h"
#include "kernel/sysinfo.h"
#include "kernel/task.h"
#include "kernel/version.h"

struct command {
    const char *name;
    const char *summary;
    const char *usage;
    const char *detail;
    bool owns_short_h;
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

int kshell_register_help(const char *name, const struct kshell_help *help, kshell_cmd_fn fn)
{
    k_size_t i;

    if (name == NULL || fn == NULL) {
        return KSHELL_ERROR;
    }

    for (i = 0; i < g_command_count; i++) {
        if (strcmp(g_commands[i].name, name) == 0) {
            g_commands[i].summary = help != NULL ? help->summary : NULL;
            g_commands[i].usage = help != NULL ? help->usage : NULL;
            g_commands[i].detail = help != NULL ? help->detail : NULL;
            g_commands[i].owns_short_h = help != NULL ? help->owns_short_h : false;
            g_commands[i].fn = fn;
            return KSHELL_OK;
        }
    }

    if (g_command_count >= KSHELL_MAX_COMMANDS) {
        /*
         * The table is full: it has to be visible, because a command that
         * is not registered answers "command not found" without saying
         * why.
         */
        kerror("kshell: the table of the commands is full, '%s' is not registered", name);
        return KSHELL_ERROR;
    }

    g_commands[g_command_count].name = name;
    g_commands[g_command_count].summary = help != NULL ? help->summary : NULL;
    g_commands[g_command_count].usage = help != NULL ? help->usage : NULL;
    g_commands[g_command_count].detail = help != NULL ? help->detail : NULL;
    g_commands[g_command_count].owns_short_h = help != NULL ? help->owns_short_h : false;
    g_commands[g_command_count].fn = fn;
    g_command_count++;

    return KSHELL_OK;
}

/*
 * The old shape of the registration: one line of help, no usage line and no
 * detail. The commands of the drivers, the network and the ports still come
 * through here, and `-h` answers them with their summary and a name to
 * start from.
 */
int kshell_register(const char *name, const char *help, kshell_cmd_fn fn)
{
    struct kshell_help info;

    info.summary = help;
    info.usage = NULL;
    info.detail = NULL;
    info.owns_short_h = false;

    return kshell_register_help(name, &info, fn);
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

    /* The serial port and the display of the board show the same. */
    k_console_write(buf, strlen(buf));
}

void kshell_set_sink(kshell_sink_fn sink)
{
    k_console_set_sink(sink);
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

    /*
     * The list has to end with a null pointer: the commands read argv[1]
     * even when there is only one argument.
     */
    if (argc < max_args) {
        argv[argc] = NULL;
    }

    return argc;
}

static struct command *find_command(const char *name);

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

int kshell_help_show(const char *name)
{
    struct command *cmd;

    if (name == NULL) {
        return KSHELL_USAGE;
    }

    cmd = find_command(name);

    if (cmd == NULL) {
        /*
         * A name that is not in the table is not a command that was used
         * wrong: it is a name the shell does not know, and `help` says so
         * with the name the person typed.
         */
        kshell_printf("help: %s: no such command. Type 'help' for the list.\n", name);
        return KSHELL_ERROR;
    }

    if (cmd->usage != NULL) {
        kshell_printf("usage: %s\n", cmd->usage);
    } else {
        /*
         * A command that was registered with one line only promises no
         * more than its name and whatever words its summary holds. A made
         * up usage line would be a promise nobody checked.
         */
        kshell_printf("usage: %s [arguments]\n", cmd->name);
    }

    if (cmd->summary != NULL) {
        kshell_printf("%s\n", cmd->summary);
    }

    if (cmd->detail != NULL) {
        kshell_printf("%s", cmd->detail);
    }

    return KSHELL_OK;
}

/* Remember one flag of the table; false when the command does not know it. */
static bool option_take(const struct kshell_option *options, k_size_t count, char letter)
{
    k_size_t i;

    for (i = 0; i < count; i++) {
        if (options[i].letter == letter) {
            if (options[i].value != NULL) {
                *options[i].value = true;
            }
            return true;
        }
    }

    return false;
}

/* The answer of a line with an option the command does not have. */
static int options_refused(const char *name, const char *option, const char *usage)
{
    kshell_printf("%s: unknown option: %s\n", name, option);

    if (usage != NULL) {
        kshell_printf("usage: %s\n", usage);
    }

    return KSHELL_OPTIONS_ERROR;
}

int kshell_options(int argc, char **argv, const struct kshell_option *options, k_size_t count,
                   const char *usage, struct kshell_args *out)
{
    bool flags_done = false;
    const char *name;
    int positionals = 0;
    int i;

    if (out == NULL || argc < 1 || argv == NULL) {
        return KSHELL_OPTIONS_ERROR;
    }

    name = argv[0];
    out->argc = 0;
    out->argv[0] = NULL;

    for (i = 1; i < argc; i++) {
        const char *word = argv[i];

        if (!flags_done && strcmp(word, "--") == 0) {
            flags_done = true;
            continue;
        }

        if (!flags_done && word[0] == '-' && word[1] == '-' && word[2] != '\0') {
            if (strcmp(word, "--help") == 0) {
                return KSHELL_OPTIONS_HELP;
            }
            return options_refused(name, word, usage);
        }

        if (!flags_done && word[0] == '-' && word[1] != '\0') {
            const char *letter = word + 1;

            for (; *letter != '\0'; letter++) {
                /*
                 * A command may have a flag of its own on the letter `h`
                 * (the human numbers of `df`): the table of the command is
                 * asked first, and `--help` stays the way to its help.
                 */
                if (option_take(options, count, *letter)) {
                    continue;
                }

                if (*letter == 'h') {
                    return KSHELL_OPTIONS_HELP;
                }

                {
                    char option[3];

                    option[0] = '-';
                    option[1] = *letter;
                    option[2] = '\0';

                    return options_refused(name, option, usage);
                }
            }
            continue;
        }

        /*
         * Every word that is not an option is an argument, and the
         * arguments are counted: a command that got one too many has to
         * say how it is used instead of ignoring the word.
         */
        if (positionals >= KSHELL_MAX_ARGS) {
            kshell_printf("%s: too many arguments\n", name);
            return KSHELL_OPTIONS_ERROR;
        }

        out->argv[positionals++] = argv[i];
    }

    out->argc = positionals;
    out->argv[positionals] = NULL;

    return KSHELL_OPTIONS_OK;
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
        kshell_printf("%s: command not found. Type 'help' for the list.\n", argv[0]);
        return KSHELL_ERROR;
    }

    /*
     * Every command answers -h and --help, the ones of the drivers too, and
     * none of them does its work for that answer. Only the first word is
     * looked at: `write /etc/x -h` writes the two characters, and a command
     * with options of its own finds the help behind them with
     * kshell_options(). A leading `--` says that even the first word is an
     * argument, and a command that claims `-h` for itself (df) is left alone.
     */
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        return kshell_help_show(argv[0]);
    }

    if (argc > 1 && !cmd->owns_short_h && strcmp(argv[1], "-h") == 0) {
        return kshell_help_show(argv[0]);
    }

    return cmd->fn(argc, argv);
}

/* --- the built in commands --- */

/*
 * The help is deliberately short: the screen of the board is small and the
 * flash is not big either. The list is one line per command; "help <command>"
 * prints the usage and the notes of that one command.
 */
static int cmd_help(int argc, char **argv)
{
    struct kshell_args args;
    k_size_t i;
    int rc;

    rc = kshell_options(argc, argv, NULL, 0, "help [command]", &args);
    if (rc == KSHELL_OPTIONS_HELP) {
        return kshell_help_show(argv[0]);
    }
    if (rc != KSHELL_OPTIONS_OK) {
        return KSHELL_USAGE;
    }

    if (args.argc > 1) {
        kshell_printf("usage: help [command]\n");
        return KSHELL_USAGE;
    }

    if (args.argc == 1) {
        return kshell_help_show(args.argv[0]);
    }

    kshell_printf("%-9s %s\n", "command", "what it does");
    for (i = 0; i < g_command_count; i++) {
        kshell_printf("%-9s %s\n", g_commands[i].name,
                      g_commands[i].summary != NULL ? g_commands[i].summary : "");
    }

    return KSHELL_OK;
}

static int cmd_uname(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    kshell_printf("%s %s %s (%s)\n", kernel_name(), kernel_version(), k_sysinfo_arch(),
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

        state = (t == self) ? "running" : task_state_name(task_state_of(t));
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

/* The longest sleep of the shell: the console belongs to the person. */
#define SLEEP_MAX_MS 60000u

static int cmd_sleep(int argc, char **argv)
{
    unsigned long ms;
    char *end = NULL;

    if (argc < 2) {
        kshell_printf("usage: sleep <milliseconds>\n");
        return KSHELL_USAGE;
    }

    /* The text has to be a whole number, not the beginning of one. */
    ms = microos_strtoul(argv[1], &end, 10);

    if (end == argv[1] || (end != NULL && *end != '\0') || microos_errno() == MICROOS_ERANGE) {
        kshell_printf("sleep: %s: not a number of milliseconds\n", argv[1]);
        return KSHELL_USAGE;
    }

    /*
     * The shell does not disappear for weeks: the sleep of a human is
     * short, and the console has to come back.
     */
    if (ms > (unsigned long)SLEEP_MAX_MS) {
        kshell_printf("sleep: %lu ms is too much, slept %u ms\n", ms, (unsigned)SLEEP_MAX_MS);
        ms = SLEEP_MAX_MS;
    }

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
    /* The print buffer of the shell is 192 bytes; the log of the boot is
     * longer, and a person wants all of it. */
    k_console_write(buf, strlen(buf));

    return KSHELL_OK;
}

static int cmd_free(int argc, char **argv)
{
    k_size_t heap_free = k_sysinfo_heap_free() / 1024;
    k_size_t heap_total = k_sysinfo_heap_total() / 1024;
#if KERNEL_PSRAM
    k_size_t psram_free = k_sysinfo_psram_free() / 1024;
#endif

    (void)argc;
    (void)argv;

    /*
     * A machine with one memory has no PSRAM line to print: a zero there
     * would be a fact about the build, not about the memory (ADR 0010).
     */
#if KERNEL_PSRAM
    kshell_printf("heap: %u KB free of %u KB, psram: %u KB free\n", (unsigned)heap_free,
                  (unsigned)heap_total, (unsigned)psram_free);
#else
    kshell_printf("heap: %u KB free of %u KB\n", (unsigned)heap_free, (unsigned)heap_total);
#endif
    kshell_printf("uptime: %llu ms, %u tick(s)\n", (unsigned long long)k_sched_uptime_ms(),
                  (unsigned)k_sched_ticks());

    return KSHELL_OK;
}

static int cmd_abi(int argc, char **argv)
{
    k_u32_t number;

    (void)argc;
    (void)argv;

    kshell_printf("the system calls of the kernel:\n");
    kshell_printf("%-5s %s\n", "nr", "name");

    for (number = 0; number < KSYS_COUNT; number++) {
        const char *name = ksys_name(number);

        if (name != NULL) {
            kshell_printf("%-5u %s\n", (unsigned)number, name);
        }
    }

    kshell_printf("%u call(s) registered\n", (unsigned)ksys_count());

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
    k_reboot();

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

/*
 * The commands of the shell itself, with the help a person reads. One table,
 * so that a usage line, the one line of the list and the notes of `-h`
 * cannot drift apart when a flag is added.
 */
static const struct builtin_command {
    const char *name;
    struct kshell_help help;
    kshell_cmd_fn fn;
} g_builtin_commands[] = {
    {"help",
     {.summary = "list the available commands, or describe one of them",
      .usage = "help [command]",
      .detail = "With a command the usage and the notes of that command are printed;\n"
                "`<command> -h` prints the same. Without one every command is listed\n"
                "with one line.\n"},
     cmd_help},
    {"uname",
     {.summary = "kernel name and version",
      .usage = "uname",
      .detail = "The name, the version, the architecture and the build of the kernel.\n"},
     cmd_uname},
    {"uptime", {.summary = "time since the boot", .usage = "uptime"}, cmd_uptime},
    {"ps", {.summary = "list the tasks", .usage = "ps"}, cmd_ps},
    {"echo", {.summary = "print the arguments", .usage = "echo [text...]"}, cmd_echo},
    {"sleep",
     {.summary = "wait for a while",
      .usage = "sleep <milliseconds>",
      .detail = "The wait is cut at 60 s: the console belongs to the person at the\n"
                "keyboard, and the shell has to come back.\n"},
     cmd_sleep},
    {"dmesg", {.summary = "the kernel log", .usage = "dmesg"}, cmd_dmesg},
    {"free", {.summary = "the memory usage", .usage = "free"}, cmd_free},
    {"clear", {.summary = "clear the screen", .usage = "clear"}, cmd_clear},
    {"modules",
     {.summary = "check the hardware modules",
      .usage = "modules",
      .detail = "Runs the doctor of the modules and prints its report; the status is an\n"
                "error when it found a problem.\n"},
     cmd_modules},
    {"abi",
     {.summary = "the system calls of the kernel",
      .usage = "abi",
      .detail = "The number and the name of every system call of this kernel.\n"},
     cmd_abi},
    {"reboot", {.summary = "restart the board", .usage = "reboot"}, cmd_reboot},
};

int kshell_init(void)
{
    k_size_t i;
    int rc = 0;

    for (i = 0; i < K_ARRAY_SIZE(g_builtin_commands); i++) {
        rc |= kshell_register_help(g_builtin_commands[i].name, &g_builtin_commands[i].help,
                                   g_builtin_commands[i].fn);
    }

    rc |= kshell_register_memtest();
    rc |= kshell_register_files();
    rc |= kshell_register_mfetch();

    return rc;
}

/*
 * Read a line from the console. Every character is repeated right away, so
 * that the person at the keyboard sees what is being typed. The backspace
 * erases the last character.
 * A line that did not fit into the buffer is not returned for execution:
 * it is a different command, and a different command deletes a different
 * file.
 */
#define LINE_TOO_LONG (-2)

static int shell_read_line(char *buf, k_size_t size)
{
    k_size_t len = 0;
    bool cut = false;

    for (;;) {
        int c = k_console_getchar();

        if (c < 0) {
            buf[len] = '\0';
            if (cut) {
                kshell_printf("line too long, it was not executed\n");
                return LINE_TOO_LONG;
            }
            return (len > 0) ? (int)len : -1;
        }

        if (c == '\n' || c == '\r') {
            k_console_write("\r\n", 2);
            buf[len] = '\0';
            if (cut) {
                /* A cut line is a different command: it is thrown away. */
                kshell_printf("line too long, it was not executed\n");
                return LINE_TOO_LONG;
            }
            return (int)len;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                k_console_write("\b \b", 3);
            }
            continue;
        }

        if (c == 3) { /* Ctrl+C: the line is thrown away */
            k_console_write("^C\r\n", 4);
            len = 0;
            cut = false;
            continue;
        }

        if (len + 1 < size) {
            buf[len++] = (char)c;
            k_console_write(&buf[len - 1], 1);
        } else {
            /* The character does not fit: from here on the line is not
             * the line any more, and it must not be executed. */
            cut = true;
        }
    }
}

void kshell_run(void)
{
    char line[KSHELL_LINE_MAX];
    int rc;

    rc = kshell_init();
    if (rc != 0) {
        /* A command of the shell is not there: say it at once. */
        kshell_printf("the shell: %d command(s) could not be registered\n", -rc);
    }

    kshell_printf("%s shell ready. Type 'help' for the list of commands.\n", kernel_name());

    for (;;) {
        int read_rc;

        kshell_printf("microos> ");

        read_rc = shell_read_line(line, sizeof(line));

        if (read_rc == -1) {
            /* The console is closed (end of the input). */
            kshell_printf("\n");
            break;
        }

        if (read_rc == LINE_TOO_LONG) {
            continue; /* the line was thrown away, the prompt comes back */
        }

        kshell_execute(line);
    }
}
