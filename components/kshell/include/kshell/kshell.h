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

#include "kernel/types.h"

#define KSHELL_MAX_COMMANDS 24
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

/* Number of registered commands. */
k_size_t kshell_command_count(void);

/* Register the memory test command. */
int kshell_register_memtest(void);

/*
 * Register the built in commands. The shell does it on its own, the tests
 * call it directly.
 */
int kshell_init(void);

#endif /* MICRO_OS_KSHELL_KSHELL_H */
