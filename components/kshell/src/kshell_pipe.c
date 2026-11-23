/*
 * Micro OS — the pipes and the redirections of the shell.
 *
 * A line is a chain of stages separated by `|`, and every stage may send its
 * input or its output to a file: `a > f`, `a >> f`, `a < f` and `a 2> f`.
 * The grammar is a line of words (the operators are words of their own, with
 * spaces around them). A word is read before the operators are looked for:
 * the quotes and the backslash of the lexer below turn `grep 'a b'` into one
 * argument, and an operator inside quotes is a character of that argument.
 *
 * The pipeline itself is the kernel ABI of ADR 0014 in use: a pipe per
 * boundary, a task per stage, dup2 of the ends onto 0, 1 and 2 in the child,
 * the unused ends closed everywhere, and the parent waits for every stage.
 * The shell is a task of the kernel rather than a program, so the stage is
 * made with task_fork_shared(): the child of a kernel task that inherits the
 * open files and the working directory, which is the honest shape of fork
 * here (a program's fork is a copy of its address space, and a kernel task
 * has none). A program of the machine is started from the stage with
 * execve(), and k_port_exec_enter() copies the table of the stage into it.
 *
 * The pipes are made one boundary at a time, as the stages are forked: the
 * shell keeps one reading end between two forks and closes the boundary
 * behind it, so a chain is limited by the line and the task table and not by
 * the sixteen descriptors of a task. A stage that is already running does not
 * inherit the pipes of the boundaries after it, because the shell makes them
 * after the fork.
 *
 * Ctrl+C belongs to the whole chain. The stages and the programs they start
 * share one job (struct task, ADR 0017), the shell is the only reader of the
 * console while it waits, and one key signals every task of the job. A stage
 * that runs a program does not poll the console for itself: its caller owns
 * the job and does that for all of them.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "kshell_internal.h"

#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "libc/unistd.h"

/*
 * The stages of one line. The line is at most 127 characters and 32 words,
 * and a stage is a command and the `|` before it, so sixteen stages are the
 * most a line of this shell can hold. The number is the table of the parser
 * and no longer a table of descriptors: the pipes are made one at a time as
 * the stages are started, and the shell keeps the two ends of one boundary
 * between two forks instead of all of them at once. A chain of sixteen
 * stages is twelve descriptors fewer than a machine whose shell held every
 * pipe end would need.
 */
#define KSHELL_STAGES_MAX 16

/* A line of 127 characters and words of one letter cannot hold more. */
#define KSHELL_WORDS_MAX 32

/* The line has more words than the table of the parser keeps. */
#define WORDS_TOO_MANY (-1)

/* An open quote or a backslash with nothing behind it. */
#define WORDS_UNTERMINATED (-2)

#define KSHELL_PIPE_PATH_MAX 128

/*
 * The two numbers a boundary that landed on 0, 1 or 2 is moved to: the top of
 * the table of the descriptors of a task (TASK_MAX_FDS is sixteen). The shell
 * holds a handful of open files and a pipe takes the lowest free numbers, so
 * the two highest slots are free in the child.
 */
#define KSHELL_STAGE_IN_MOVE (TASK_MAX_FDS - 1)
#define KSHELL_STAGE_OUT_MOVE (TASK_MAX_FDS - 2)

/* One redirection of a stage: the word after the operator and its shape. */
struct pipe_redirection {
    const char *path;
    bool append;
    bool set;
};

struct pipe_stage {
    char *argv[KSHELL_MAX_ARGS + 1];
    int argc;
    struct pipe_redirection in;
    struct pipe_redirection out;
    struct pipe_redirection err;
};

struct pipeline {
    struct pipe_stage stages[KSHELL_STAGES_MAX];
    int count;
};

/*
 * What a stage task needs: the stage of the line and the two ends of the
 * pipes it was started with. The numbers are copied and not looked up: the
 * stage runs while the shell makes the pipe of the next boundary, and the
 * table of the pipeline is the shell's.
 */
struct stage_start {
    struct pipe_stage *stage;
    int in_fd;
    int out_fd;
};

/* --- the words and the grammar --- */

/*
 * Cut the line into words, in place, and remember which of them came out of
 * quotes or an escape: such a word is an argument even when its text is an
 * operator, so `echo '|'` prints a bar and does not start a pipeline.
 *
 * The answer is the count, WORDS_TOO_MANY when the table of the parser is
 * full, or WORDS_UNTERMINATED with `error` naming the open quote. The word
 * itself is written at the read cursor or behind it, so a word shrinks over
 * the text that is already read and nothing is copied twice; a terminator is
 * written only after the delimiter is stepped over, because at the last word
 * the two cursors are the same byte.
 */
static int split_words(char *line, char **words, bool *literal, int max, const char **error)
{
    int count = 0;
    char *read = line;
    char *write = line;

    for (;;) {
        bool quoted = false;

        while (*read == ' ' || *read == '\t') {
            read++;
        }

        if (*read == '\0') {
            break;
        }

        if (count >= max) {
            return WORDS_TOO_MANY;
        }

        words[count] = write;

        while (*read != '\0' && *read != ' ' && *read != '\t') {
            /*
             * Single quotes are the literal of POSIX: nothing inside them is
             * special, not even a backslash.
             */
            if (*read == '\'') {
                quoted = true;
                read++;

                while (*read != '\0' && *read != '\'') {
                    *write++ = *read++;
                }

                if (*read != '\'') {
                    *error = "unterminated quote";
                    return WORDS_UNTERMINATED;
                }

                read++;
                continue;
            }

            /*
             * Double quotes group the spaces and keep a backslash for `"` and
             * `\` alone, as the shell of POSIX does: `"\n"` is the two
             * characters, and `"\""` is the quote itself.
             */
            if (*read == '"') {
                quoted = true;
                read++;

                while (*read != '\0' && *read != '"') {
                    if (*read == '\\' && (read[1] == '"' || read[1] == '\\')) {
                        read++;
                    }

                    *write++ = *read++;
                }

                if (*read != '"') {
                    *error = "unterminated quote";
                    return WORDS_UNTERMINATED;
                }

                read++;
                continue;
            }

            /*
             * Outside the quotes a backslash takes the character after it as
             * it is: a space, an operator or a quote becomes part of the
             * word, and a word with one is an argument of its own.
             */
            if (*read == '\\') {
                if (read[1] == '\0') {
                    *error = "a backslash at the end of the line escapes nothing";
                    return WORDS_UNTERMINATED;
                }

                quoted = true;
                read++;
                *write++ = *read++;
                continue;
            }

            *write++ = *read++;
        }

        /* The whitespace of the separator is read before the word is ended;
         * the two cursors may be on the same byte and a terminator written
         * there would hide the rest of the line from the reader. */
        while (*read == ' ' || *read == '\t') {
            read++;
        }

        *write++ = '\0';
        literal[count++] = quoted;
    }

    return count;
}

/*
 * The words the shell does not parse yet. They are named instead of being
 * read as a program or as a file name, because `a && b` silently run as the
 * command `a` with two arguments is a different line from the one typed.
 */
static bool unsupported_word(const char *word)
{
    return strcmp(word, "&&") == 0 || strcmp(word, "||") == 0 || strcmp(word, ";") == 0 ||
           strcmp(word, "&") == 0 || strcmp(word, "2>&1") == 0;
}

/* The redirection an operator word asks for; false when it is no operator. */
static bool redirection_word(const char *word, int *fd, bool *append)
{
    if (strcmp(word, ">") == 0) {
        *fd = 1;
        *append = false;
        return true;
    }
    if (strcmp(word, ">>") == 0) {
        *fd = 1;
        *append = true;
        return true;
    }
    if (strcmp(word, "<") == 0) {
        *fd = 0;
        *append = false;
        return true;
    }
    if (strcmp(word, "2>") == 0) {
        *fd = 2;
        *append = false;
        return true;
    }
    if (strcmp(word, "2>>") == 0) {
        *fd = 2;
        *append = true;
        return true;
    }

    return false;
}

static struct pipe_redirection *stage_redirection(struct pipe_stage *stage, int fd)
{
    if (fd == 0) {
        return &stage->in;
    }
    if (fd == 2) {
        return &stage->err;
    }

    return &stage->out;
}

/* The words of the line to the stages; false after printing the reason. A
 * word that came out of quotes is never an operator: it is an argument or a
 * file name, whatever its text says. */
static bool pipeline_parse(struct pipeline *p, char **words, const bool *literal, int word_count)
{
    int index = 0;
    int at = 0;
    char *word;
    bool quoted;

    memset(p, 0, sizeof(*p));

    index = 0;

    while (at < word_count) {
        struct pipe_stage *stage = &p->stages[index];
        int fd;
        bool append;

        word = words[at];
        quoted = literal[at];

        if (!quoted && unsupported_word(word)) {
            kshell_eprintf("the shell: '%s' is not supported\n", word);
            return false;
        }

        if (!quoted && strcmp(word, "|") == 0) {
            if (stage->argc == 0) {
                kshell_eprintf("the shell: a pipe needs a command on both sides\n");
                return false;
            }

            if (index + 1 >= KSHELL_STAGES_MAX) {
                kshell_eprintf("the shell: too many stages (at most %d)\n", KSHELL_STAGES_MAX);
                return false;
            }

            index++;
            at++;
            continue;
        }

        if (!quoted && redirection_word(word, &fd, &append)) {
            struct pipe_redirection *redirection = stage_redirection(stage, fd);

            if (at + 1 >= word_count) {
                kshell_eprintf("the shell: %s needs a file name\n", word);
                return false;
            }

            if (redirection->set) {
                kshell_eprintf("the shell: descriptor %d is redirected twice\n", fd);
                return false;
            }

            at++;

            if ((!literal[at] && redirection_word(words[at], &fd, &append)) ||
                (!literal[at] && (strcmp(words[at], "|") == 0 || unsupported_word(words[at])))) {
                kshell_eprintf("the shell: %s needs a file name\n", word);
                return false;
            }

            redirection->path = words[at];
            redirection->append = append;
            redirection->set = true;
            at++;
            continue;
        }

        if (stage->argc >= KSHELL_MAX_ARGS) {
            kshell_eprintf("the shell: too many arguments in one stage\n");
            return false;
        }

        stage->argv[stage->argc++] = word;
        at++;
    }

    if (p->stages[index].argc == 0) {
        kshell_eprintf("the shell: a pipe needs a command on both sides\n");
        return false;
    }

    p->count = index + 1;

    for (index = 0; index < p->count; index++) {
        p->stages[index].argv[p->stages[index].argc] = NULL;
    }

    return true;
}

/* --- the stage --- */

static bool stage_apply_redirection(struct pipe_stage *stage,
                                    const struct pipe_redirection *redirection, int target)
{
    char path[KSHELL_PIPE_PATH_MAX];
    int flags;
    int fd;

    if (!kshell_resolve_path(redirection->path, path, sizeof(path))) {
        kshell_eprintf("%s: %s: path too long\n", stage->argv[0], redirection->path);
        return false;
    }

    if (target == 0) {
        flags = MICROOS_O_RDONLY;
    } else {
        flags = MICROOS_O_WRONLY | MICROOS_O_CREAT;
        flags |= redirection->append ? MICROOS_O_APPEND : MICROOS_O_TRUNC;
    }

    fd = microos_open(path, flags);
    if (fd < 0) {
        kshell_eprintf("%s: %s: %s\n", stage->argv[0], path, microos_strerror(microos_errno()));
        return false;
    }

    if (microos_dup2(fd, target) != target) {
        kshell_eprintf("%s: %s: %s\n", stage->argv[0], path, microos_strerror(microos_errno()));
        microos_close(fd);
        return false;
    }

    if (fd != target) {
        microos_close(fd);
    }

    return true;
}

/*
 * Put the ends of the pipes on 0 and 1 and the files where the line said.
 * The task of the stage was forked with the two ends of its own boundary and
 * nothing else - the shell closes each end before it makes the next pipe - so
 * every descriptor above 2 is closed here. A write end that nobody closed
 * keeps the reader of the pipe from ever seeing the end of the file, and a
 * read end of a writer keeps a broken pipe from being reported.
 *
 * The shell keeps no descriptors of its own - the console of the kernel is 1
 * and 2 when they are not open as files - so `pipe()` hands out the low
 * numbers and an end of a boundary can sit on 0, 1 or 2. A read end that
 * lands on 1 would be the standard output of the stage and every write of it
 * would be a `bad file descriptor`; the ends are therefore moved above 2 and
 * the three low numbers are closed before the copies are made. The copies
 * then own exactly 0 and 1, 2 is the console again unless the line said
 * otherwise, and a chain of any length starts its stages the same way.
 *
 * The output and the error are set before the input, so a file that cannot
 * be opened is reported where the line asked for the errors.
 */
static bool stage_setup(struct pipe_stage *stage, int in_fd, int out_fd)
{
    int fd;

    /* The two ends, above the three numbers the copies need. 14 and 15 are
     * the two highest slots of TASK_MAX_FDS: the shell holds a handful of
     * descriptors and never a pipe end there. */
    if (in_fd >= 0 && in_fd <= 2) {
        if (microos_dup2(in_fd, KSHELL_STAGE_IN_MOVE) != KSHELL_STAGE_IN_MOVE) {
            return false;
        }
        microos_close(in_fd);
        in_fd = KSHELL_STAGE_IN_MOVE;
    }

    if (out_fd >= 0 && out_fd <= 2) {
        if (microos_dup2(out_fd, KSHELL_STAGE_OUT_MOVE) != KSHELL_STAGE_OUT_MOVE) {
            return false;
        }
        microos_close(out_fd);
        out_fd = KSHELL_STAGE_OUT_MOVE;
    }

    /* Whatever the shell had at 0, 1 and 2 belongs to the shell: the stage
     * starts without them, and the copies below are the only owners. */
    for (fd = 0; fd <= 2; fd++) {
        microos_close(fd);
    }

    if (in_fd >= 0 && microos_dup2(in_fd, 0) != 0) {
        return false;
    }

    if (out_fd >= 0 && microos_dup2(out_fd, 1) != 1) {
        return false;
    }

    /* The two ends are at 0 and 1 now; the copies are the stage's own
     * descriptors and the numbers they came from are not. The inherited
     * table above 2 belongs to the shell and is not part of the stage. */
    for (fd = 3; fd < TASK_MAX_FDS; fd++) {
        microos_close(fd);
    }

    if (stage->err.set && !stage_apply_redirection(stage, &stage->err, 2)) {
        return false;
    }
    if (stage->out.set && !stage_apply_redirection(stage, &stage->out, 1)) {
        return false;
    }
    if (stage->in.set && !stage_apply_redirection(stage, &stage->in, 0)) {
        return false;
    }

    return true;
}

/*
 * The body of one stage. A builtin runs here, in the task of the stage, and
 * its output follows descriptor 1 because kshell_printf() writes there; a
 * program of the machine is started with execve() and inherits the same
 * three descriptors. A command that changes the state of the shell has no
 * meaning in a task of its own and is refused by the runner before this.
 */
static void stage_task(void *arg)
{
    struct stage_start *start = (struct stage_start *)arg;
    int status;

    if (!stage_setup(start->stage, start->in_fd, start->out_fd)) {
        task_exit(1);

        /* Not reached: task_exit() ends the task. */
        for (;;) {
        }
    }

    status = kshell_invoke(start->stage->argc, start->stage->argv, true);

    task_exit(status);

    /* Not reached: task_exit() ends the task. */
    for (;;) {
    }
}

/* --- the pipeline --- */

/* Wait for one stage and read the console meanwhile. The answer is the
 * status of the stage; a signal of the job ends every stage of the chain. */
static int pipeline_wait(k_u32_t pid, k_u32_t job)
{
    int status = 1;

    for (;;) {
        int rc = task_wait(pid, &status, 5);

        if (rc == 0) {
            return status;
        }

        if (rc != -2) {
            return 1;
        }

        /* Ctrl+C of the console is the end of the whole chain, not of the
         * stage that happens to be the one being waited for. */
        if (k_console_poll() == 3) {
            (void)task_signal_job(job, K_SIGINT);
        }
    }
}

static int pipeline_run(struct pipeline *p)
{
    struct task *self = task_self();
    struct stage_start starts[KSHELL_STAGES_MAX];
    k_u32_t pids[KSHELL_STAGES_MAX];
    k_u32_t job;
    k_u32_t previous_job;
    int status = 0;
    int started = 0;
    int index;
    int in_fd = -1;
    int cost = 0;
    k_size_t free_tasks;

    if (self == NULL) {
        kshell_eprintf("the shell: a pipeline needs a task\n");
        return KSHELL_ERROR;
    }

    /*
     * The task table is the real bound of a chain, so the chain is counted
     * before the first pipe is made: a stage that runs a program is two
     * tasks (the stage waits, the program runs) and a stage that is a command
     * of the shell is one. A chain that does not fit is refused whole, with
     * the arithmetic in the sentence - a chain that starts and then loses
     * its last stages is a line whose answer nobody can trust.
     */
    for (index = 0; index < p->count; index++) {
        cost += kshell_stage_is_program(p->stages[index].argv[0]) ? 2 : 1;
    }

    free_tasks = (k_size_t)TASK_MAX_TASKS - task_count();

    if ((k_size_t)cost > free_tasks) {
        kshell_eprintf("the shell: %d stages need %d task slots and %u of %u are free\n", p->count,
                       cost, (unsigned)free_tasks, (unsigned)TASK_MAX_TASKS);
        kshell_eprintf("the shell: a stage that runs a program is two tasks (the stage and the "
                       "program); shorten the chain\n");
        return KSHELL_ERROR;
    }

    /*
     * The job is the identity of the chain for as long as it runs. The task
     * of the shell owns no job of its own outside the line: the stages
     * inherit this value and the shell takes its old one back before it
     * waits, so a later line is a different job.
     */
    job = self->pid;
    previous_job = task_get_job(self);
    task_set_job(self, job);

    /*
     * One boundary at a time. A pipe is made, the stage before it is forked
     * with its output end, and the shell keeps the reading end for the stage
     * after it; the ends of the boundary before it are closed here, and the
     * task table of the shell never holds more than the two ends of one
     * boundary. The stages that were forked already do not inherit the pipes
     * of the boundaries behind them, because those were made after the fork.
     */
    for (index = 0; index < p->count; index++) {
        char name[TASK_NAME_MAX];
        struct task *task;
        int out_fd = -1;
        int next_in = -1;

        if (index + 1 < p->count) {
            int fds[2];

            if (microos_pipe(fds) != 0) {
                kshell_eprintf("the shell: cannot make a pipe: %s\n",
                               microos_strerror(microos_errno()));
                break;
            }

            out_fd = fds[1];
            next_in = fds[0];
        }

        snprintf(name, sizeof(name), "pipe%d", index + 1);
        starts[index].stage = &p->stages[index];
        starts[index].in_fd = in_fd;
        starts[index].out_fd = out_fd;

        task = task_fork_shared(self, name, stage_task, &starts[index]);
        if (task == NULL) {
            if (in_fd >= 0) {
                microos_close(in_fd);
            }
            if (out_fd >= 0) {
                microos_close(out_fd);
            }
            if (next_in >= 0) {
                microos_close(next_in);
            }
            in_fd = -1;
            break;
        }

        pids[index] = task->pid;
        started++;

        if (in_fd >= 0) {
            microos_close(in_fd);
        }
        if (out_fd >= 0) {
            microos_close(out_fd);
        }
        in_fd = next_in;
    }

    task_set_job(self, previous_job);

    /* The parent keeps no end of a pipe: the reader reaches the end of the
     * file when the writer of the last stage is gone, and not before. */
    if (in_fd >= 0) {
        microos_close(in_fd);
    }

    if (started < p->count) {
        kshell_eprintf("the shell: cannot start stage %d\n", started + 1);

        for (index = 0; index < started; index++) {
            (void)pipeline_wait(pids[index], job);
        }

        return KSHELL_ERROR;
    }

    /* The status of the line is the status of the last stage, as POSIX has
     * it: a producer that failed is a line that still delivered its output. */
    for (index = 0; index < p->count; index++) {
        status = pipeline_wait(pids[index], job);
    }

    return status;
}

/* --- the line --- */

int kshell_line_run(char *line)
{
    char *words[KSHELL_WORDS_MAX];
    bool literal[KSHELL_WORDS_MAX];
    struct pipeline pipeline;
    const char *error = NULL;
    int word_count;
    int index;

    word_count = split_words(line, words, literal, KSHELL_WORDS_MAX, &error);
    if (word_count == WORDS_TOO_MANY) {
        kshell_eprintf("the shell: too many words in the line\n");
        return KSHELL_USAGE;
    }

    if (word_count == WORDS_UNTERMINATED) {
        /* The line is refused whole: a guess at where the quote ends would
         * run a command the person did not type. */
        kshell_eprintf("the shell: %s\n", error);
        return KSHELL_USAGE;
    }

    if (word_count == 0) {
        return KSHELL_OK;
    }

    if (!pipeline_parse(&pipeline, words, literal, word_count)) {
        return KSHELL_USAGE;
    }

    /*
     * A line of one command and no redirection is the ordinary prompt: it
     * runs in the task of the shell, which is the only task that can change
     * the directory or PATH of the person.
     */
    if (pipeline.count == 1 && !pipeline.stages[0].in.set && !pipeline.stages[0].out.set &&
        !pipeline.stages[0].err.set) {
        return kshell_invoke(pipeline.stages[0].argc, pipeline.stages[0].argv, false);
    }

    for (index = 0; index < pipeline.count; index++) {
        if (kshell_command_is_shell_state(pipeline.stages[index].argv[0])) {
            kshell_eprintf("%s: cannot run in a pipeline or with a redirection\n",
                           pipeline.stages[index].argv[0]);
            return KSHELL_USAGE;
        }
    }

    return pipeline_run(&pipeline);
}
