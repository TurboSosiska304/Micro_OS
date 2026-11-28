/*
 * Micro OS — the shell.
 *
 * A small command interpreter, close to a Linux shell: commands are
 * registered by name, the line is split into arguments (with `|` and the
 * redirections of ADR 0017 between the stages) and the command handler
 * decides what to do. Other components (the drivers for example) can
 * register their own commands.
 */
#ifndef MICRO_OS_KSHELL_KSHELL_H
#define MICRO_OS_KSHELL_KSHELL_H

#include "kernel/console.h"
#include "kernel/types.h"

/*
 * The number of the commands of the shell. The board registers 47 today:
 * the built in ones, the drivers (the date, the bus, the radio, the
 * display, the infrared, the sound detector), the filesystem (mkfs), telnet
 * and mfetch. The simulator adds the stubs of the host and the commands of
 * the network, and the table keeps room for them and for the next phases; a
 * command that does not fit is reported at the boot instead of disappearing
 * silently. At 48 the shell dropped `exec` and `mfetch` once the commands of
 * P3 arrived, which is why the table has room now.
 */
#define KSHELL_MAX_COMMANDS 72
#define KSHELL_MAX_ARGS 8
#define KSHELL_LINE_MAX 128

/* Return codes of a command handler. */
#define KSHELL_OK 0
#define KSHELL_ERROR (-1)
#define KSHELL_USAGE (-2)
/*
 * The command ended the session it ran in (`logout`). It is not a failure:
 * the loop of a session stops and the task that runs it ends, and the init
 * that made the session starts another one on the console. A command of a
 * script or a stage of a pipe answers it as any other status.
 */
#define KSHELL_LOGOUT (-3)

/*
 * A command is not called for `-h` or `--help` as its first word: the shell
 * answers the help of the table itself, returns KSHELL_OK and does nothing
 * else. A command with options of its own asks kshell_options() about the
 * help that stands behind them.
 */

typedef int (*kshell_cmd_fn)(int argc, char **argv);

/*
 * Put a path of the command line together with the directory of the task: a
 * path that begins with '/' is itself, anything else hangs from the current
 * directory. False when the result does not fit, because a cut path names
 * another file. The commands that open a file by the name a person typed use
 * this; a path of the kernel, already absolute, does not need it.
 */
bool kshell_resolve_path(const char *path, char *out, k_size_t size);

/*
 * What the help of a command says. It lives in the table of the commands and
 * not in the handler, so that `-h`, `help <command>` and the list of `help`
 * cannot disagree after a flag was added, renamed or dropped.
 *
 * `detail` is printed as it is and ends with a newline; it holds the lines
 * that the usage line cannot: what a flag means, what the command refuses.
 */
struct kshell_help {
    const char *summary; /* one line, in the list of the commands */
    const char *usage;   /* after "usage: ", the name of the command first */
    const char *detail;  /* a few lines for `-h` and `help <command>` */
    /*
     * True when the command has a flag of its own written `-h` (the human
     * numbers of `df`): the word then reaches the command, and `--help` is
     * the way to the help of that command. A command that claims the letter
     * has to say so in its detail, because every other command follows the
     * rule that -h is the help.
     */
    bool owns_short_h;
};

/* Register a command with one line of help and no usage line of its own. */
int kshell_register(const char *name, const char *help, kshell_cmd_fn fn);

/* Register a command with the whole help of the table. */
int kshell_register_help(const char *name, const struct kshell_help *help, kshell_cmd_fn fn);

/*
 * Print the usage, the summary and the detail of a command. This is the
 * answer of `-h`, `--help` and `help <command>`: the help was asked for and
 * given, so the status is KSHELL_OK, and no command does its work for it.
 * An unknown name answers KSHELL_ERROR and says that there is no such
 * command.
 */
int kshell_help_show(const char *name);

/* The words of a command line, after the options were taken out. */
struct kshell_args {
    int argc;
    char *argv[KSHELL_MAX_ARGS + 1];
};

/* One option of a command: the letter of "-l" and where to remember it. */
struct kshell_option {
    char letter;
    bool *value;
};

/* The answers of kshell_options(); they are not the statuses of a command. */
#define KSHELL_OPTIONS_OK 0
#define KSHELL_OPTIONS_HELP 1
#define KSHELL_OPTIONS_ERROR 2

/*
 * Take the options out of a command line. The helper understands `--` (every
 * word after it is an argument, even one that begins with a dash), `-h` and
 * `--help` (KSHELL_OPTIONS_HELP: the caller prints the help with
 * kshell_help_show(), the request is not a failure), groups of short letters
 * like "-la", and nothing else: an unknown option prints the usage line of
 * `usage` and answers KSHELL_OPTIONS_ERROR, which the caller returns as
 * KSHELL_USAGE.
 */
int kshell_options(int argc, char **argv, const struct kshell_option *options, k_size_t count,
                   const char *usage, struct kshell_args *out);

/* Remove a command. */
int kshell_unregister(const char *name);

/* Execute one command line. Returns the result of the handler. */
int kshell_execute(const char *line);

/* The command loop: reads lines from the console and executes them. */
void kshell_run(void);

/* Print a formatted message to the console. */
void kshell_printf(const char *fmt, ...) K_FORMAT(__printf__, 1, 2);

/*
 * The sink of the output: the shell writes to it as well as to the console.
 * The console of the display of the board uses it.
 */
typedef k_console_sink_fn kshell_sink_fn;
void kshell_set_sink(kshell_sink_fn sink);

/* Number of registered commands. */
k_size_t kshell_command_count(void);

/* Register the memory test command. */
int kshell_register_memtest(void);

/* Register `mfetch`, the report about the system with the logo. */
int kshell_register_mfetch(void);

/* Register `date`, the wall clock of the kernel as a command (UTC). */
int kshell_register_time(void);

/*
 * A moment of the wall clock as the line of `date` - `YYYY-MM-DD HH:MM:SS` of
 * UTC - or false when the number is outside the calendar the command reads
 * (1970..2099, which is the chip of the board and the epoch of the seconds).
 * The pure half of the command, so that a test can check the calendar of a
 * fixed number without a clock of the host under it.
 */
bool kshell_time_line(k_u64_t unix_seconds, char *buf, k_size_t size);

/*
 * Register the built in commands. The shell does it on its own, the tests
 * call it directly.
 */
int kshell_init(void);

/* Register the commands that work with the files. */
int kshell_register_files(void);

#endif /* MICRO_OS_KSHELL_KSHELL_H */
