/*
 * Micro OS — the lists, pipes, redirections and jobs of the shell.
 *
 * A line is a sequence of pipelines separated by `;`, `&&` or `||`; `&`
 * starts a pipeline in the background. A pipeline is a chain of stages
 * separated by `|`, and every stage may send its input, output or error to a
 * file: `a > f`, `a >> f`, `a < f`, `a 2> f` and `a 2>&1`. The operators are
 * words of their own, with spaces around them. A word is read before the
 * operators are looked for: quotes and the backslash of the lexer turn
 * `grep 'a b'` into one argument, and an operator inside quotes is a character
 * of that argument.
 *
 * The pipeline itself is the kernel ABI of ADR 0014 in use: a pipe per
 * boundary, a task per stage, dup2 of the ends onto 0, 1 and 2 in the child,
 * the unused ends closed everywhere, and the parent waits for every stage.
 * The shell is a task of the kernel rather than a program, so the stage is
 * made with task_fork_shared(): the child of a kernel task inherits the open
 * files and the working directory. A program of the machine is started from
 * the stage with execve().
 *
 * The pipes are made one boundary at a time, as the stages are forked: the
 * shell keeps one reading end between two forks and closes the boundary
 * behind it. A stage that is already running does not inherit the pipes of
 * the boundaries after it, because the shell makes them after the fork.
 *
 * A pipeline is also one job: its stages and the programs they start share a
 * process group and a job number. The shell waits for a foreground job and
 * polls the console while it waits; Ctrl+C ends the job and Ctrl+Z stops it.
 * A background job is entered in the shell's table and reaped by `jobs`,
 * `fg` and `bg` rather than by the line that started it.
 */
#include "kshell/kshell.h"

#include <stdio.h>
#include <string.h>

#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/tty.h"
#include "libc/unistd.h"

#include "kshell_internal.h"

/*
 * The stages of one pipeline. The line is at most 127 characters and 32
 * words, and a stage is a command and the `|` before it, so sixteen stages
 * are the most a line can hold. The pipes are made one at a time as the
 * stages are started, so this is a parser bound and not a descriptor bound.
 */
#define KSHELL_STAGES_MAX 16

/* A line of 127 characters and words of one letter cannot hold more. */
#define KSHELL_WORDS_MAX 32

/* A list has at least one word and one separating operator per item. */
#define KSHELL_LIST_MAX 16

/* The job table is per shell task; four entries keep a console useful without
 * spending the board's scarce internal SRAM on sixteen idle pipelines. */
#define KSHELL_JOBS_MAX 4

/* The line has more words than the table of the parser keeps. */
#define WORDS_TOO_MANY (-1)

/* An open quote or a backslash with nothing behind it. */
#define WORDS_UNTERMINATED (-2)

#define KSHELL_PIPE_PATH_MAX 128

/*
 * The two numbers a boundary that landed on 0, 1 or 2 is moved to: the top of
 * the table of the descriptors of a task (TASK_MAX_FDS is sixteen).
 */
#define KSHELL_STAGE_IN_MOVE (TASK_MAX_FDS - 1)
#define KSHELL_STAGE_OUT_MOVE (TASK_MAX_FDS - 2)

/* The status a stopped foreground job leaves as the answer of the line. */
#define KSHELL_JOB_STOPPED_STATUS (128 + K_SIGTSTP)

/* One redirection of a stage: a file, or a duplicate of another descriptor. */
struct pipe_redirection {
    const char *path;
    int source_fd;
    bool append;
    bool duplicate;
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

/* What a stage task needs. The numbers are copied and not looked up: the
 * stage runs while the shell makes the pipe of the next boundary. */
struct stage_start {
    struct pipe_stage *stage;
    int in_fd;
    int out_fd;
    k_u32_t pgrp; /* zero for the first stage: it becomes its own leader */
};

enum job_state {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE,
};

/* One job of one shell task. The pids are the stage tasks; a program started
 * by a stage is a child of that stage and is reaped there. */
struct shell_job {
    bool used;
    k_u32_t owner;
    k_u32_t number;
    k_u32_t pid;
    k_u32_t pgrp;
    char text[KSHELL_LINE_MAX];
    enum job_state state;
    int count;
    k_u32_t pids[KSHELL_STAGES_MAX];
    bool reaped[KSHELL_STAGES_MAX];
    int last_status;
    struct stage_start starts[KSHELL_STAGES_MAX];
    struct pipeline pipeline;
    char storage[KSHELL_LINE_MAX];
};

static struct shell_job g_jobs[KSHELL_JOBS_MAX];
static k_u32_t g_next_job = 1;

enum list_operator {
    LIST_SEQUENCE,
    LIST_AND,
    LIST_OR,
};

/* One pipeline and the list operator that follows it. The words stay in the
 * caller's line; the range is parsed once to validate the whole line and once
 * more when the item runs. */
struct list_item {
    int first;
    int last;
    enum list_operator after;
    bool background;
};

/* --- the words and the grammar --- */

/*
 * Whether the unquoted cursor starts a list operator. Single `|` stays a
 * word unless it is separated by whitespace, as the shell's pipe grammar
 * requires; the list operators also work without spaces (`a;b`, `a&&b`). The
 * one ampersand that is not a list operator is the middle of `2>&1`.
 */
static bool list_boundary(const char *read, const char *token)
{
    if (*read == ';') {
        return true;
    }
    if (*read == '&') {
        return !(read == token + 2 && token[0] == '2' && token[1] == '>');
    }
    return *read == '|' && read[1] == '|';
}

/*
 * Cut the line into words in a separate buffer, and remember which of them
 * came out of quotes or an escape: such a word is an argument even when its
 * text is an operator, so `echo '|'` prints a bar and does not start a
 * pipeline. The separate buffer lets an attached operator end one word
 * without overwriting the byte the reader has not consumed yet.
 */
static int split_words(const char *line, char *storage, char **words, bool *literal, int max,
                       const char **error)
{
    int count = 0;
    const char *read = line;
    char *write = storage;

    for (;;) {
        bool quoted = false;
        bool operator_word = false;
        const char *token_read;

        while (*read == ' ' || *read == '\t') {
            read++;
        }

        if (*read == '\0') {
            break;
        }

        if (count >= max) {
            return WORDS_TOO_MANY;
        }

        token_read = read;
        words[count] = write;

        if (*read == ';') {
            *write++ = *read++;
            operator_word = true;
        } else if (*read == '&') {
            *write++ = *read++;
            if (*read == '&') {
                *write++ = *read++;
            }
            operator_word = true;
        } else if (*read == '|' && read[1] == '|') {
            *write++ = *read++;
            *write++ = *read++;
            operator_word = true;
        }

        while (!operator_word && *read != '\0' && *read != ' ' && *read != '\t' &&
               !list_boundary(read, token_read)) {
            /* Single quotes are the literal of POSIX. */
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

            /* Inside double quotes a backslash escapes a quote or a backslash
             * alone, as the shell of POSIX does. */
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

            /* Outside the quotes a backslash takes the next character as it
             * is: a space, an operator or a quote becomes part of the word. */
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

        *write++ = '\0';
        literal[count++] = operator_word ? false : quoted;
    }

    return count;
}

/* The list operator an unquoted word asks for, or zero for an ordinary word. */
static char list_operator(const char *word, bool quoted)
{
    if (quoted) {
        return '\0';
    }
    if (strcmp(word, ";") == 0) {
        return ';';
    }
    if (strcmp(word, "&&") == 0) {
        return '&';
    }
    if (strcmp(word, "||") == 0) {
        return '|';
    }
    if (strcmp(word, "&") == 0) {
        return '&';
    }

    return '\0';
}

/* The redirection an operator word asks for; false when it is no operator. */
static bool redirection_word(const char *word, int *fd, bool *append, bool *duplicate,
                             int *source_fd)
{
    *duplicate = false;
    *source_fd = -1;

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
    if (strcmp(word, "2>&1") == 0) {
        *fd = 2;
        *append = false;
        *duplicate = true;
        *source_fd = 1;
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

/* The words of one pipeline to its stages. The range excludes the list
 * operator that follows it. */
static bool pipeline_parse(struct pipeline *p, char **words, const bool *literal, int first,
                           int last)
{
    int index = 0;
    int at = first;

    memset(p, 0, sizeof(*p));

    while (at < last) {
        struct pipe_stage *stage = &p->stages[index];
        char *word = words[at];
        bool quoted = literal[at];
        int fd;
        bool append;
        bool duplicate;
        int source_fd;

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

        if (!quoted && redirection_word(word, &fd, &append, &duplicate, &source_fd)) {
            struct pipe_redirection *redirection = stage_redirection(stage, fd);

            if (redirection->set) {
                kshell_eprintf("the shell: descriptor %d is redirected twice\n", fd);
                return false;
            }

            redirection->path = NULL;
            redirection->source_fd = source_fd;
            redirection->append = append;
            redirection->duplicate = duplicate;
            redirection->set = true;

            if (duplicate) {
                at++;
                continue;
            }

            if (at + 1 >= last) {
                kshell_eprintf("the shell: %s needs a file name\n", word);
                return false;
            }

            at++;

            if (!literal[at] &&
                (redirection_word(words[at], &fd, &append, &duplicate, &source_fd) ||
                 strcmp(words[at], "|") == 0 || list_operator(words[at], false) != '\0')) {
                kshell_eprintf("the shell: %s needs a file name\n", word);
                return false;
            }

            redirection->path = words[at];
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

/*
 * Validate the whole list before any command runs. A trailing `;` is the
 * empty separator of POSIX and is accepted; an empty command between two
 * operators is not. `&&` and `||` bind their two neighbours, and `&` ends the
 * pipeline before it and starts the next list item after it.
 */
static bool list_parse(struct list_item *items, int *count, char **words, const bool *literal,
                       int word_count)
{
    struct pipeline pipeline;
    int first = 0;
    int at = 0;
    int used = 0;

    while (at < word_count) {
        char op = list_operator(words[at], literal[at]);

        if (op == '\0') {
            at++;
            continue;
        }

        if (at == first) {
            kshell_eprintf("the shell: '%s' needs a command before it\n", words[at]);
            return false;
        }

        if (used >= KSHELL_LIST_MAX) {
            kshell_eprintf("the shell: too many commands in the list (at most %d)\n",
                           KSHELL_LIST_MAX);
            return false;
        }

        items[used].first = first;
        items[used].last = at;
        items[used].background = (op == '&' && strcmp(words[at], "&") == 0);
        items[used].after = (strcmp(words[at], "&&") == 0)   ? LIST_AND
                            : (strcmp(words[at], "||") == 0) ? LIST_OR
                                                             : LIST_SEQUENCE;
        used++;

        if (strcmp(words[at], "&&") == 0 || strcmp(words[at], "||") == 0) {
            if (at + 1 >= word_count) {
                kshell_eprintf("the shell: '%s' needs a command after it\n", words[at]);
                return false;
            }
            if (list_operator(words[at + 1], literal[at + 1]) != '\0') {
                kshell_eprintf("the shell: '%s' needs a command after it\n", words[at]);
                return false;
            }
        }

        at++;
        first = at;

        if (at < word_count && list_operator(words[at], literal[at]) != '\0') {
            kshell_eprintf("the shell: an empty command between list operators\n");
            return false;
        }
    }

    if (first < word_count) {
        if (used >= KSHELL_LIST_MAX) {
            kshell_eprintf("the shell: too many commands in the list (at most %d)\n",
                           KSHELL_LIST_MAX);
            return false;
        }

        items[used].first = first;
        items[used].last = word_count;
        items[used].after = LIST_SEQUENCE;
        items[used].background = false;
        used++;
    }

    if (used == 0) {
        /* The caller handles an empty line; this is a defensive refusal. */
        kshell_eprintf("the shell: a list needs a command\n");
        return false;
    }

    for (at = 0; at < used; at++) {
        if (!pipeline_parse(&pipeline, words, literal, items[at].first, items[at].last)) {
            return false;
        }
    }

    *count = used;
    return true;
}

/* Join the words of one item for the job table. Quotes have done their work
 * already; the text is a readable label, not a line to parse a second time. */
static void command_text(char **words, int first, int last, char *out, k_size_t size)
{
    k_size_t used = 0;
    int at;

    if (size == 0) {
        return;
    }

    for (at = first; at < last; at++) {
        k_size_t len = strlen(words[at]);
        k_size_t room = size - 1u - used;

        if (at > first) {
            if (room == 0) {
                break;
            }
            out[used++] = ' ';
            room--;
        }

        if (len > room) {
            len = room;
        }
        memcpy(out + used, words[at], len);
        used += len;
    }

    out[used] = '\0';
}

/* --- the stage --- */

static bool stage_apply_redirection(struct pipe_stage *stage,
                                    const struct pipe_redirection *redirection, int target)
{
    char path[KSHELL_PIPE_PATH_MAX];
    struct task *self;
    int flags;
    int fd;

    if (redirection->duplicate) {
        self = task_self();
        if (self == NULL || redirection->source_fd < 0 || redirection->source_fd >= TASK_MAX_FDS) {
            return false;
        }

        /* A null descriptor is the console fallback of this kernel. There is
         * no kfile to duplicate, and leaving the closed target null keeps the
         * same fallback. */
        if (self->files[redirection->source_fd] == NULL) {
            return true;
        }

        if (microos_dup2(redirection->source_fd, target) != target) {
            kshell_eprintf("%s: %d>&%d: %s\n", stage->argv[0], target, redirection->source_fd,
                           microos_strerror(microos_errno()));
            return false;
        }

        return true;
    }

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
 * The output is installed before `2>&1`, so the duplicate names the output
 * the person asked for. A redirection of the error to a file is installed
 * before the output is opened, so a failure to open the output is reported in
 * that file rather than on the console.
 */
static bool stage_setup(struct pipe_stage *stage, int in_fd, int out_fd)
{
    int fd;

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

    /* The inherited table above 2 belongs to the shell and is not part of
     * the stage. */
    for (fd = 3; fd < TASK_MAX_FDS; fd++) {
        microos_close(fd);
    }

    if (stage->err.set && !stage->err.duplicate &&
        !stage_apply_redirection(stage, &stage->err, 2)) {
        return false;
    }
    if (stage->out.set && !stage_apply_redirection(stage, &stage->out, 1)) {
        return false;
    }
    if (stage->err.set && stage->err.duplicate && !stage_apply_redirection(stage, &stage->err, 2)) {
        return false;
    }
    if (stage->in.set && !stage_apply_redirection(stage, &stage->in, 0)) {
        return false;
    }

    return true;
}

/* A builtin runs in the task of the stage and writes descriptor 1 through
 * kshell_printf(); a program started with execve() inherits the same three
 * descriptors. */
static void stage_task(void *arg)
{
    struct stage_start *start = (struct stage_start *)arg;
    struct task *self = task_self();
    int status;

    /* The parent may still be returning from fork on another core. Put this
     * task in its final group before it can start a program, so the program
     * never inherits the shell's group by accident. */
    if (self != NULL) {
        k_u32_t pgrp = start->pgrp != 0 ? start->pgrp : self->pid;
        (void)task_setpgid(self->pid, pgrp);
    }

    if (!stage_setup(start->stage, start->in_fd, start->out_fd)) {
        task_exit(1);
        for (;;) {
        }
    }

    status = kshell_invoke(start->stage->argc, start->stage->argv, true);
    task_exit(status);
    for (;;) {
    }
}

/* --- the job table --- */

static const char *job_state_name(enum job_state state)
{
    if (state == JOB_STOPPED) {
        return "Stopped";
    }
    if (state == JOB_DONE) {
        return "Done";
    }

    return "Running";
}

/* Reap the stage tasks that have finished and derive the state from the ones
 * that have not. A stopped stage is enough to stop the displayed job: its
 * program and the other stages received the same group signal. */
static void job_refresh(struct shell_job *job)
{
    int index;
    bool alive = false;
    bool stopped = false;

    if (!job->used) {
        return;
    }

    for (index = 0; index < job->count; index++) {
        int status = 1;
        int rc;

        if (job->reaped[index]) {
            continue;
        }

        rc = task_wait(job->pids[index], &status, 0);
        if (rc == 0 || rc == -1) {
            job->reaped[index] = true;
            job->last_status = status;
            continue;
        }

        {
            struct task *task = task_by_pid(job->pids[index]);

            if (task != NULL && task_state_of(task) == TASK_STATE_STOPPED) {
                stopped = true;
            } else {
                alive = true;
            }
        }
    }

    if (stopped) {
        job->state = JOB_STOPPED;
    } else if (!alive) {
        job->state = JOB_DONE;
    } else {
        job->state = JOB_RUNNING;
    }
}

static void jobs_refresh(void)
{
    k_u32_t owner = task_self() != NULL ? task_self()->pid : 0;
    k_size_t index;

    for (index = 0; index < KSHELL_JOBS_MAX; index++) {
        if (g_jobs[index].used && g_jobs[index].owner == owner) {
            job_refresh(&g_jobs[index]);
        }
    }
}

static struct shell_job *job_find(k_u32_t number)
{
    k_u32_t owner = task_self() != NULL ? task_self()->pid : 0;
    k_size_t index;

    jobs_refresh();

    for (index = 0; index < KSHELL_JOBS_MAX; index++) {
        if (g_jobs[index].used && g_jobs[index].owner == owner && g_jobs[index].number == number) {
            return &g_jobs[index];
        }
    }

    return NULL;
}

static struct shell_job *job_allocate(const char *text)
{
    k_u32_t owner = task_self() != NULL ? task_self()->pid : 0;
    struct shell_job *slot = NULL;
    k_size_t index;

    jobs_refresh();

    for (index = 0; index < KSHELL_JOBS_MAX; index++) {
        if (!g_jobs[index].used) {
            slot = &g_jobs[index];
            break;
        }
    }

    /* A finished job has already been reaped; its number may be reused when
     * every entry is occupied by completed work. */
    if (slot == NULL) {
        for (index = 0; index < KSHELL_JOBS_MAX; index++) {
            if (g_jobs[index].used && g_jobs[index].owner == owner &&
                g_jobs[index].state == JOB_DONE) {
                slot = &g_jobs[index];
                break;
            }
        }
    }

    if (slot == NULL) {
        return NULL;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->owner = owner;
    slot->number = g_next_job++;
    if (g_next_job == 0) {
        g_next_job = 1;
    }
    slot->state = JOB_RUNNING;
    slot->last_status = KSHELL_OK;

    {
        k_size_t length = strlen(text);
        if (length >= sizeof(slot->text)) {
            length = sizeof(slot->text) - 1u;
        }
        memcpy(slot->text, text, length);
        slot->text[length] = '\0';
    }

    return slot;
}

static void job_print(const struct shell_job *job, const char *state)
{
    kshell_printf("[%u] pid %u pgrp %u %-7s %s\n", (unsigned)job->number, (unsigned)job->pid,
                  (unsigned)job->pgrp, state, job->text);
}

/* A background stage outlives the line that made it. Keep its argv and file
 * names in the job entry rather than pointing at the line's stack buffer. */
static char *job_store_word(struct shell_job *job, k_size_t *used, const char *word)
{
    k_size_t length = strlen(word) + 1u;
    char *stored;

    if (length > sizeof(job->storage) - *used) {
        return NULL;
    }

    stored = job->storage + *used;
    memcpy(stored, word, length);
    *used += length;
    return stored;
}

static bool job_copy_pipeline(struct shell_job *job, const struct pipeline *source)
{
    k_size_t used = 0;
    int stage;

    job->pipeline = *source;

    for (stage = 0; stage < source->count; stage++) {
        int arg;
        struct pipe_redirection *redirects[3] = {&job->pipeline.stages[stage].in,
                                                 &job->pipeline.stages[stage].out,
                                                 &job->pipeline.stages[stage].err};

        for (arg = 0; arg < source->stages[stage].argc; arg++) {
            char *word = job_store_word(job, &used, source->stages[stage].argv[arg]);
            if (word == NULL) {
                return false;
            }
            job->pipeline.stages[stage].argv[arg] = word;
        }

        for (arg = 0; arg < 3; arg++) {
            if (redirects[arg]->set && !redirects[arg]->duplicate) {
                char *path = job_store_word(job, &used, redirects[arg]->path);
                if (path == NULL) {
                    return false;
                }
                redirects[arg]->path = path;
            }
        }
    }

    return true;
}

/* Wait for one stage and read the console meanwhile. True means Ctrl+Z (or a
 * terminal-generated SIGTSTP) stopped the job; the caller restores the tty. */
static bool pipeline_wait_one(k_u32_t pid, k_u32_t pgrp, int *status)
{
    *status = 1;

    for (;;) {
        struct task *task = task_by_pid(pid);
        int rc;

        if (task != NULL && task_state_of(task) == TASK_STATE_STOPPED) {
            return true;
        }

        rc = task_wait(pid, status, 5);
        if (rc == 0) {
            return false;
        }
        if (rc == -1) {
            return false;
        }

        if (!k_tty_console_claimed()) {
            int key = k_console_poll();

            if (key == 3) {
                (void)task_signal_pgrp(pgrp, K_SIGINT);
            } else if (key == K_TTY_DEF_SUSP) {
                kshell_printf("^Z\n");
                (void)task_signal_pgrp(pgrp, K_SIGTSTP);
                return true;
            }
        }
    }
}

/* Wait for every unreaped stage of a job. The status of a pipeline is the
 * status of its last stage, as POSIX has it. */
static bool job_wait(struct shell_job *job, int *status)
{
    int index;

    *status = job->last_status;

    for (index = 0; index < job->count; index++) {
        int stage_status = 1;

        if (job->reaped[index]) {
            continue;
        }

        if (pipeline_wait_one(job->pids[index], job->pgrp, &stage_status)) {
            job->state = JOB_STOPPED;
            return true;
        }

        job->reaped[index] = true;
        job->last_status = stage_status;
        *status = stage_status;
    }

    job_refresh(job);
    *status = job->last_status;
    return false;
}

static int pipeline_run(struct pipeline *p, const char *text, bool background)
{
    struct task *self = task_self();
    k_u32_t pids[KSHELL_STAGES_MAX];
    struct shell_job *job;
    k_u32_t job_id;
    k_u32_t previous_job;
    k_u32_t pgrp = 0;
    k_u32_t self_pgid;
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

    /* A stage that runs a program is two tasks (the stage waits, the program
     * runs); a stage that is a command of the shell is one. */
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

    job = job_allocate(text);
    if (job == NULL) {
        kshell_eprintf("the shell: no free job table entry\n");
        return KSHELL_ERROR;
    }

    if (!job_copy_pipeline(job, p)) {
        kshell_eprintf("the shell: the job's command does not fit the line buffer\n");
        memset(job, 0, sizeof(*job));
        return KSHELL_ERROR;
    }
    p = &job->pipeline;

    /* The stages inherit this identity for as long as the shell starts them;
     * the shell takes its old one back before it waits or answers the line. */
    job_id = self->pid;
    previous_job = task_get_job(self);
    self_pgid = task_get_pgid(self);
    task_set_job(self, job_id);

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
        job->starts[index].stage = &p->stages[index];
        job->starts[index].in_fd = in_fd;
        job->starts[index].out_fd = out_fd;
        job->starts[index].pgrp = pgrp;

        task = task_fork_shared(self, name, stage_task, &job->starts[index]);
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

        if (pgrp == 0) {
            pgrp = task->pid;
            (void)task_setpgid(task->pid, pgrp);
            if (!background) {
                (void)k_tty_set_fg_pgrp(pgrp);
            }
        } else {
            (void)task_setpgid(task->pid, pgrp);
        }

        if (in_fd >= 0) {
            microos_close(in_fd);
        }
        if (out_fd >= 0) {
            microos_close(out_fd);
        }
        in_fd = next_in;
    }

    task_set_job(self, previous_job);

    if (in_fd >= 0) {
        microos_close(in_fd);
    }

    job->pid = started > 0 ? pids[0] : 0;
    job->pgrp = pgrp;
    job->count = started;

    for (index = 0; index < started; index++) {
        job->pids[index] = pids[index];
    }

    if (started < p->count) {
        kshell_eprintf("the shell: cannot start stage %d\n", started + 1);

        if (started > 0) {
            int ignored;
            (void)task_signal_pgrp(pgrp, K_SIGTERM);
            for (index = 0; index < started; index++) {
                (void)task_wait(pids[index], &ignored, 1000);
            }
        }

        if (!background) {
            (void)k_tty_set_fg_pgrp(self_pgid != 0 ? self_pgid : self->pid);
        }
        memset(job, 0, sizeof(*job));
        return KSHELL_ERROR;
    }

    if (background) {
        kshell_printf("[%u] %u\n", (unsigned)job->number, (unsigned)job->pid);
        return KSHELL_OK;
    }

    if (job_wait(job, &status)) {
        (void)k_tty_set_fg_pgrp(self_pgid != 0 ? self_pgid : self->pid);
        return KSHELL_JOB_STOPPED_STATUS;
    }

    (void)k_tty_set_fg_pgrp(self_pgid != 0 ? self_pgid : self->pid);
    memset(job, 0, sizeof(*job));
    return status;
}

/* --- the builtins of the job table --- */

static bool job_number(const char *text, k_u32_t *number)
{
    k_u32_t value = 0;
    k_size_t at = 1;

    if (text == NULL || text[0] != '%' || text[1] == '\0') {
        return false;
    }

    for (; text[at] != '\0'; at++) {
        unsigned digit = (unsigned char)text[at] - (unsigned)'0';
        if (digit > 9u || value > (0xFFFFFFFFu - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }

    if (value == 0) {
        return false;
    }

    *number = value;
    return true;
}

static struct shell_job *current_job(void)
{
    k_u32_t owner = task_self() != NULL ? task_self()->pid : 0;
    struct shell_job *latest = NULL;
    k_size_t index;

    jobs_refresh();

    for (index = 0; index < KSHELL_JOBS_MAX; index++) {
        struct shell_job *job = &g_jobs[index];

        if (job->used && job->owner == owner && job->state != JOB_DONE &&
            (latest == NULL || job->number > latest->number)) {
            latest = job;
        }
    }

    return latest;
}

static int cmd_jobs(int argc, char **argv)
{
    k_u32_t owner = task_self() != NULL ? task_self()->pid : 0;
    k_size_t index;

    if (argc != 1) {
        kshell_printf("usage: jobs\n");
        return KSHELL_USAGE;
    }

    jobs_refresh();

    for (index = 0; index < KSHELL_JOBS_MAX; index++) {
        if (g_jobs[index].used && g_jobs[index].owner == owner) {
            job_print(&g_jobs[index], job_state_name(g_jobs[index].state));
        }
    }

    return KSHELL_OK;
}

static int cmd_fg(int argc, char **argv)
{
    struct task *self = task_self();
    struct shell_job *job;
    k_u32_t number = 0;
    k_u32_t self_pgid;
    int status;

    if (argc > 2 || (argc == 2 && !job_number(argv[1], &number))) {
        kshell_printf("usage: fg [%%N]\n");
        return KSHELL_USAGE;
    }

    job = (argc == 2) ? job_find(number) : current_job();
    if (job == NULL || job->state == JOB_DONE) {
        kshell_printf("fg: %s: no such job\n", argc == 2 ? argv[1] : "%N");
        return KSHELL_ERROR;
    }

    if (self == NULL || k_tty_set_fg_pgrp(job->pgrp) != 0) {
        kshell_eprintf("fg: cannot take the terminal for job %u\n", (unsigned)job->number);
        return KSHELL_ERROR;
    }

    self_pgid = task_get_pgid(self);
    job_print(job, "Continued");
    (void)task_signal_pgrp(job->pgrp, K_SIGCONT);
    job->state = JOB_RUNNING;

    if (job_wait(job, &status)) {
        (void)k_tty_set_fg_pgrp(self_pgid != 0 ? self_pgid : self->pid);
        return KSHELL_JOB_STOPPED_STATUS;
    }

    (void)k_tty_set_fg_pgrp(self_pgid != 0 ? self_pgid : self->pid);
    return status;
}

static int cmd_bg(int argc, char **argv)
{
    struct shell_job *job;
    k_u32_t number = 0;

    if (argc > 2 || (argc == 2 && !job_number(argv[1], &number))) {
        kshell_printf("usage: bg [%%N]\n");
        return KSHELL_USAGE;
    }

    job = (argc == 2) ? job_find(number) : current_job();
    if (job == NULL || job->state == JOB_DONE) {
        kshell_printf("bg: %s: no such job\n", argc == 2 ? argv[1] : "%N");
        return KSHELL_ERROR;
    }

    (void)task_signal_pgrp(job->pgrp, K_SIGCONT);
    job->state = JOB_RUNNING;
    job_print(job, "Running");

    return KSHELL_OK;
}

int kshell_jobs_command(const char *name, int argc, char **argv)
{
    if (strcmp(name, "jobs") == 0) {
        return cmd_jobs(argc, argv);
    }
    if (strcmp(name, "fg") == 0) {
        return cmd_fg(argc, argv);
    }
    if (strcmp(name, "bg") == 0) {
        return cmd_bg(argc, argv);
    }

    return KSHELL_ERROR;
}

/* --- the line --- */

int kshell_line_run(char *line)
{
    char *words[KSHELL_WORDS_MAX];
    bool literal[KSHELL_WORDS_MAX];
    char storage[KSHELL_LINE_MAX];
    struct list_item items[KSHELL_LIST_MAX];
    struct pipeline pipeline;
    const char *error = NULL;
    int word_count;
    int item_count;
    int index;
    int status = KSHELL_OK;

    word_count = split_words(line, storage, words, literal, KSHELL_WORDS_MAX, &error);
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

    if (!list_parse(items, &item_count, words, literal, word_count)) {
        return KSHELL_USAGE;
    }

    for (index = 0; index < item_count; index++) {
        struct list_item *item = &items[index];
        bool skip = false;

        if (index > 0 && items[index - 1].after == LIST_AND && status != KSHELL_OK) {
            skip = true;
        }
        if (index > 0 && items[index - 1].after == LIST_OR && status == KSHELL_OK) {
            skip = true;
        }

        if (skip) {
            continue;
        }

        if (!pipeline_parse(&pipeline, words, literal, item->first, item->last)) {
            return KSHELL_USAGE;
        }

        /*
         * A line of one command and no redirection is the ordinary prompt:
         * it runs in the task of the shell, which is the only task that can
         * change its directory, PATH or limits. `exec` is the exception: its
         * stage starts a program and needs the same group as a pipeline so
         * Ctrl+C and Ctrl+Z reach that program.
         */
        if (pipeline.count == 1 && !pipeline.stages[0].in.set && !pipeline.stages[0].out.set &&
            !pipeline.stages[0].err.set && !item->background &&
            strcmp(pipeline.stages[0].argv[0], "exec") != 0) {
            status = kshell_invoke(pipeline.stages[0].argc, pipeline.stages[0].argv, false);
            continue;
        }

        for (int stage = 0; stage < pipeline.count; stage++) {
            if (kshell_command_is_shell_state(pipeline.stages[stage].argv[0])) {
                kshell_eprintf("%s: cannot run in a pipeline or with a redirection\n",
                               pipeline.stages[stage].argv[0]);
                return KSHELL_USAGE;
            }
        }

        {
            char text[KSHELL_LINE_MAX];
            command_text(words, item->first, item->last, text, sizeof(text));
            status = pipeline_run(&pipeline, text, item->background);
        }
    }

    return status;
}
