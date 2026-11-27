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

/*
 * Run the words as a program when one of that name is in PATH. The answer is
 * 1 when the program was found and run (the status is written where `status`
 * points), and 0 when the shell has to answer with its own command: no file
 * of that name, or a machine whose port cannot enter a program. A file that
 * exists but cannot run is answered here (1 and an error).
 */
int kshell_try_program(int argc, char **argv, int *status);

/* Resolve a path against the working directory of the calling task. */
bool kshell_resolve_path(const char *path, char *out, k_size_t size);

/* The size of a path a caller of the two functions below keeps. */
#define KSHELL_PATH_MAX 128

/*
 * The programs of PATH, for `help`: the executable files of the directories
 * of PATH, a bounded number of names and `...` when there are more. A build
 * without the exec path has no program to run, so it lists nothing.
 */
void kshell_help_list_programs(void);

/*
 * The file of PATH that `name` runs, when the machine has a program of that
 * name: the full path is written where `out` points and the answer is true.
 * A name with a slash is not looked up; a file without an execute bit does
 * not answer, as in the runner.
 */
bool kshell_help_find_program(const char *name, char *out, k_size_t size);

/* Whether a program of that name is in PATH - the same question as above
 * without the path, for the arithmetic of the task table of a pipeline. */
bool kshell_help_has_program(const char *name);

/*
 * The same question for a stage of a line: true when the stage runs a file of
 * the machine rather than a command of the shell. A stage like that is two
 * tasks of the kernel, because execve() starts the program in a task of its
 * own and the stage waits for it.
 */
bool kshell_stage_is_program(const char *name);

/* Write bytes to the console of the caller (descriptor 1), waiting out a
 * pipe that is full. The big reports use this; kshell_printf() is the same
 * with a format. */
void kshell_write(const char *data, k_size_t len);

/* The diagnostics of a stage: descriptor 2, which is the console unless the
 * line redirected it. */
void kshell_eprintf(const char *fmt, ...);

/*
 * Parse and run a line: the words, the lists, the pipes and the redirections.
 * The caller keeps its own copy because the lexer uses a bounded work buffer
 * for the words and the job table keeps the text of background commands.
 */
int kshell_line_run(char *line);

/* Run `jobs`, `fg` or `bg` in the task that owns the job table. The name is
 * the first word of the command; the remaining words are its arguments. */
int kshell_jobs_command(const char *name, int argc, char **argv);

#endif /* MICRO_OS_KSHELL_INTERNAL_H */
