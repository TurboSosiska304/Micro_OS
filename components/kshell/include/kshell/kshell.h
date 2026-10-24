/*
 * Micro OS — the shell.
 *
 * A small command interpreter, close to a Linux shell: commands are
 * registered by name, the line is split into arguments and the command
 * handler decides what to do. Other components (the drivers for example)
 * can register their own commands.
 */
#ifndef MICRO_OS_KSHELL_KSHELL_H
#define MICRO_OS_KSHELL_KSHELL_H

#include "kernel/console.h"
#include "kernel/types.h"

/*
 * The number of the commands of the shell. The built in commands are 23,
 * the drivers add their own (the date, the bus, the radio, the display)
 * and the filesystem its mkfs, so the table has room for all of them and
 * for the commands of the next phases.
 */
#define KSHELL_MAX_COMMANDS 32
#define KSHELL_MAX_ARGS 8
#define KSHELL_LINE_MAX 128

/* Return codes of a command handler. */
#define KSHELL_OK 0
#define KSHELL_ERROR (-1)
#define KSHELL_USAGE (-2)

typedef int (*kshell_cmd_fn)(int argc, char **argv);

/* Register a command. Returns 0 on success. */
int kshell_register(const char *name, const char *help, kshell_cmd_fn fn);

/* Remove a command. */
int kshell_unregister(const char *name);

/* Execute one command line. Returns the result of the handler. */
int kshell_execute(const char *line);

/* The command loop: reads lines from the console and executes them. */
void kshell_run(void);

/* Print a formatted message to the console. */
void kshell_printf(const char *fmt, ...);

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

/*
 * Register the built in commands. The shell does it on its own, the tests
 * call it directly.
 */
int kshell_init(void);

/* Register the commands that work with the files. */
int kshell_register_files(void);

#endif /* MICRO_OS_KSHELL_KSHELL_H */
