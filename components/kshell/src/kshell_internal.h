/*
 * Micro OS — the parts of the shell that one source of the shell shares with
 * another, and with nothing else.
 *
 * The public header (kshell/kshell.h) is what a driver or a port registers a
 * command through. This one is the seam between the interpreter, the commands
 * of the files and the runner of a pipeline: it is not installed and no
 * component outside components/kshell includes it.
 */
#ifndef MICRO_OS_KSHELL_INTERNAL_H
#define MICRO_OS_KSHELL_INTERNAL_H

#include "kernel/types.h"

/*
 * Run one command of the table with the words of a line. A name that is not
 * in the table of the commands is a program when `child` is true (the stage
 * of a pipeline): it is looked up in PATH or taken as a path, and the status
 * is the status of the stage. A name that is not a program is reported.
 */
int kshell_invoke(int argc, char **argv, bool child);

/*
 * True for the commands that change the state of the shell itself (the
 * working directory, PATH): a pipeline stage runs in a task of its own, and
 * such a command there would change a copy nobody sees - a trap, not a
 * feature. The runner refuses them with a sentence.
 */
bool kshell_command_is_shell_state(const char *name);

/* Run the words of a child as a program (PATH or a path); the status is the
 * status of a stage. Implemented with the files of the shell. */
int kshell_run_program(int argc, char **argv);

/* Resolve a path against the working directory of the calling task. */
bool kshell_resolve_path(const char *path, char *out, k_size_t size);

/* Write bytes to the console of the caller (descriptor 1), waiting out a
 * pipe that is full. The big reports use this; kshell_printf() is the same
 * with a format. */
void kshell_write(const char *data, k_size_t len);

/* The diagnostics of a stage: descriptor 2, which is the console unless the
 * line redirected it. */
void kshell_eprintf(const char *fmt, ...);

/*
 * Parse and run a line: the words, the pipes and the redirections. The line
 * is written over (the words are cut in place), so the caller keeps its own
 * copy if it needs it.
 */
int kshell_line_run(char *line);

#endif /* MICRO_OS_KSHELL_INTERNAL_H */
