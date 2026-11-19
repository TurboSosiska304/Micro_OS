/*
 * Micro OS — the init process and the scripts of /etc/init.d.
 *
 * The board has no execve: a program is a command of the shell. A script of
 * the init process is therefore a list of shell commands, one per line, and
 * every script runs in a task of its own. A daemon that never returns does
 * not hold the init process, and a script that returns lets its task end.
 */
#ifndef MICRO_OS_INIT_INITD_H
#define MICRO_OS_INIT_INITD_H

#include "kernel/types.h"

/* The directory of the scripts of the init process. */
#define INITD_DIR "/etc/init.d"

/* The most a script may weigh: the boot must not wait for a huge file. */
#define INITD_SCRIPT_MAX 4096

/* The most scripts a directory may hold in one run. */
#define INITD_MAX_SCRIPTS 8

/*
 * Start the scripts of `dir`, each in a task of its own. The directory and
 * its parents are created when they are missing, so the scripts of a fresh
 * volume have a place to live. Returns the number of the scripts started, or
 * a negative error when the directory cannot be read.
 */
int initd_run(const char *dir);

/*
 * Run one script in the current task. Empty lines and lines that start with
 * '#' are skipped; every other line is a command of the shell. Returns 0 when
 * the script was read (a command that fails is reported in the log), or a
 * negative error when the file cannot be opened or read.
 */
int initd_run_script(const char *path);

#endif /* MICRO_OS_INIT_INITD_H */
