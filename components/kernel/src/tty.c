/*
 * Micro OS — the terminal of a program: the mode of a line and the questions
 * of `termios` and `ioctl` (ADR 0051).
 *
 * The bytes themselves come from `kernel/console.h`, which already knows the
 * difference between the console of the machine and the terminal of a task
 * (kernel/console.h, `struct k_terminal`). What is here is the discipline
 * between the bytes and the program: a line at a time with the echo and the
 * editing of a terminal, or the raw bytes of a screen that draws itself, with
 * the four combinations of VMIN and VTIME that POSIX defines.
 *
 * The mode belongs to the terminal. The machine has one console - the
 * structure `g_console` below - and a task with a terminal of its own (a
 * session of telnet) has one of its own, made on the first question and freed
 * with the task. Two programs that open the console share its mode, and the
 * console goes back to canonical when the program that changed it goes away.
 */
#include "kernel/tty.h"

#include <string.h>

#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/wait.h"

/* The answers of the reading below a byte: a byte is 0..255, and these are
 * the three states between the console and the line discipline. */
#define TTY_NONE (-1)   /* nothing waits right now */
#define TTY_GONE (-2)   /* the terminal is gone: the end of the file */
#define TTY_SIGNAL (-3) /* a signal cut the wait */

/*
 * The length of one sleep of a read that waits for the console: the serial
 * line of x86-64 has no interrupt of its own and the byte is found by a poll,
 * so a wait is a chunk that ends in another poll (ADR 0051 §8).
 */
#define K_TTY_WAIT_CHUNK_MS 10u

/* What one byte did to the line. */
#define TTY_MORE 0  /* the line goes on */
#define TTY_READY 1 /* the line is complete */
#define TTY_EOF 2   /* the end of the file of this terminal */

struct k_tty {
    struct k_tty_termios mode;

    /*
     * The line of the canonical mode: the bytes of a line being typed, the
     * position the reader has taken them from, and whether the line is
     * complete. A line that is longer than the buffer is cut and the rest of
     * it is dropped to the newline, the way the shell drops an overlong
     * command: a cut line is a different line.
     */
    char line[K_TTY_LINE_MAX];
    k_u32_t length;
    k_u32_t position;
    bool ready;
    bool cut;
    bool eof;

    /*
     * The byte `poll` found and nobody has read yet. A poll of the console
     * used to take the byte out of the ring of the keyboard to answer "yes";
     * the answer is kept here instead, so that a program that polls its
     * terminal and then reads it gets the key it was told about.
     */
    int pushback;

    /*
     * The task that owns this terminal while it is a program's: it asked for
     * the mode or it is reading, and until it goes away the keys are its own.
     * The caller of `execve` asks `k_tty_console_claimed()` before it takes a
     * key for Ctrl+C (ADR 0051 §8).
     */
    k_u32_t owner;
};

/*
 * The console of the machine: the terminal of every task that has none. It is
 * made in canonical mode with the echo here and not only by k_tty_init(),
 * because a build that never boots the kernel (the tests of the host) must
 * still find a terminal of a Unix.
 */
static struct k_tty g_console = {
    .mode =
        {
            .c_iflag = K_TTY_ICRNL | K_TTY_IXON,
            .c_oflag = K_TTY_OPOST | K_TTY_ONLCR,
            .c_cflag = K_TTY_CS8 | K_TTY_CREAD,
            .c_lflag =
                K_TTY_ISIG | K_TTY_ICANON | K_TTY_ECHO | K_TTY_ECHOE | K_TTY_ECHOK | K_TTY_IEXTEN,
            .c_line = 0,
            .c_cc =
                {
                    [K_TTY_VINTR] = K_TTY_DEF_INTR,
                    [K_TTY_VQUIT] = K_TTY_DEF_QUIT,
                    [K_TTY_VERASE] = K_TTY_DEF_ERASE,
                    [K_TTY_VKILL] = K_TTY_DEF_KILL,
                    [K_TTY_VEOF] = K_TTY_DEF_EOF,
                    [K_TTY_VSTART] = K_TTY_DEF_START,
                    [K_TTY_VSTOP] = K_TTY_DEF_STOP,
                    [K_TTY_VSUSP] = K_TTY_DEF_SUSP,
                    [K_TTY_VMIN] = 1,
                    [K_TTY_VTIME] = 0,
                },
        },
    .pushback = -1,
};

static void tty_init(struct k_tty *tty)
{
    memset(tty, 0, sizeof(*tty));
    k_tty_defaults(&tty->mode);
    tty->pushback = -1;
}

/* The terminal of the calling task: the console, or the one of a task that
 * has a terminal of its own. NULL only when the block cannot be made. */
static struct k_tty *tty_current(void)
{
    struct task *self = task_self();

    if (self == NULL || self->terminal == NULL) {
        return &g_console;
    }

    if (self->tty_state == NULL) {
        struct k_tty *tty = kmm_alloc(sizeof(*tty), KMM_PRIORITY_USER);

        if (tty == NULL) {
            return NULL;
        }

        tty_init(tty);
        self->tty_state = tty;
    }

    return self->tty_state;
}

static bool tty_is_raw(const struct k_tty *tty)
{
    return (tty->mode.c_lflag & K_TTY_ICANON) == 0;
}

/* --- the defaults --- */

void k_tty_defaults(struct k_tty_termios *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    /*
     * The mode a terminal of a Unix starts in: a line at a time, with the
     * echo, with the signal of Ctrl+C, and with the two translations a
     * keyboard needs (a CR is a NL). OPOST says that the output is
     * post-processed, which is true of the console: it breaks the line itself.
     */
    out->c_iflag = K_TTY_ICRNL | K_TTY_IXON;
    out->c_oflag = K_TTY_OPOST | K_TTY_ONLCR;
    out->c_cflag = K_TTY_CS8 | K_TTY_CREAD;
    out->c_lflag =
        K_TTY_ISIG | K_TTY_ICANON | K_TTY_ECHO | K_TTY_ECHOE | K_TTY_ECHOK | K_TTY_IEXTEN;
    out->c_line = 0;
    out->c_cc[K_TTY_VINTR] = K_TTY_DEF_INTR;
    out->c_cc[K_TTY_VQUIT] = K_TTY_DEF_QUIT;
    out->c_cc[K_TTY_VERASE] = K_TTY_DEF_ERASE;
    out->c_cc[K_TTY_VKILL] = K_TTY_DEF_KILL;
    out->c_cc[K_TTY_VEOF] = K_TTY_DEF_EOF;
    out->c_cc[K_TTY_VSTART] = K_TTY_DEF_START;
    out->c_cc[K_TTY_VSTOP] = K_TTY_DEF_STOP;
    out->c_cc[K_TTY_VSUSP] = K_TTY_DEF_SUSP;
    out->c_cc[K_TTY_VMIN] = 1;
    out->c_cc[K_TTY_VTIME] = 0;
}

void k_tty_make_raw(struct k_tty_termios *settings)
{
    if (settings == NULL) {
        return;
    }

    /* The recipe of `cfmakeraw`: no translation of the input, no break, no
     * parity, no flow control, no post-processing of the output, eight bits,
     * no canonical mode, no echo, no signals and no extensions, and a read
     * that returns each byte as it comes. */
    settings->c_iflag &= ~(K_TTY_IGNBRK | K_TTY_BRKINT | K_TTY_PARMRK | K_TTY_ISTRIP | K_TTY_INLCR |
                           K_TTY_IGNCR | K_TTY_ICRNL | K_TTY_IXON);
    settings->c_oflag &= ~K_TTY_OPOST;
    settings->c_lflag &= ~(K_TTY_ECHO | K_TTY_ECHONL | K_TTY_ICANON | K_TTY_ISIG | K_TTY_IEXTEN);
    settings->c_cflag &= ~(K_TTY_CSIZE | K_TTY_PARENB);
    settings->c_cflag |= K_TTY_CS8;
    settings->c_cc[K_TTY_VMIN] = 1;
    settings->c_cc[K_TTY_VTIME] = 0;
}

/* --- the bytes --- */

/*
 * One byte of the terminal: the byte, or TTY_NONE when the deadline passed,
 * or TTY_GONE when the terminal is not there any more, or TTY_SIGNAL when a
 * signal has to be answered before the call returns.
 *
 * A console is polled and then slept on - the key wakes the task through
 * K_WAIT_CHANNEL_CONSOLE - and a terminal of telnet is read with its own
 * `get`, which waits by itself and cannot be asked whether a byte is there
 * (docs/debts.md). A timeout is therefore exact for the console and coarse
 * for a socket.
 */
static int tty_wait_byte(struct k_tty *tty, k_u32_t timeout_ms)
{
    const bool infinite = (timeout_ms == K_WAIT_INFINITE);
    k_u64_t deadline = infinite ? 0 : k_sched_uptime_ms() + (k_u64_t)timeout_ms;
    struct task *self = task_self();

    if (self != NULL && self->terminal != NULL && self->terminal->get != NULL) {
        int c = self->terminal->get(self->terminal->ctx);

        return (c < 0) ? TTY_GONE : c;
    }

    for (;;) {
        k_u32_t wait = K_TTY_WAIT_CHUNK_MS;
        int c;

        if (tty->pushback >= 0) {
            c = tty->pushback;
            tty->pushback = -1;
            return c;
        }

        c = k_port_console_poll();

        if (c >= 0) {
            return c;
        }

        if (self == NULL) {
            /* The boot context has no run queue to leave and no key to wait
             * for: a read of the terminal of the boot is not a thing. */
            return TTY_NONE;
        }

        if (!infinite) {
            k_u64_t now = k_sched_uptime_ms();

            if (now >= deadline) {
                return TTY_NONE;
            }
            if ((k_u64_t)wait > deadline - now) {
                wait = (k_u32_t)(deadline - now);
            }
        }

        /*
         * The wait is a chunk and not the whole timeout, because the serial
         * line of this port has no interrupt of its own: a byte that arrives
         * on COM1 is found by the poll above and by nothing else, so a task
         * that slept for ever would sleep past its own key. The console of
         * the board and the keyboard of x86-64 wake the task through
         * K_WAIT_CHANNEL_CONSOLE and the chunk costs them nothing.
         */
        switch (k_wait_block(K_WAIT_CHANNEL_CONSOLE, NULL, wait)) {
        case K_WAIT_WOKEN:
            continue; /* look at the console again */
        case K_WAIT_SIGNAL:
            return TTY_SIGNAL;
        default:
            continue; /* the chunk passed: poll, and sleep again if quiet */
        }
    }
}

/* Whether the tty repeats what the person types. A screen and a UART do not
 * echo, so the console of the port echoes here; a terminal of telnet answers
 * the question the session asked of the client (RFC 857). */
static bool tty_echoes(const struct k_tty *tty)
{
    if ((tty->mode.c_lflag & K_TTY_ECHO) == 0) {
        return false;
    }

    return k_console_terminal_echo();
}

static void tty_echo(const char *data, k_size_t len)
{
    k_console_terminal_put(data, len);
}

/* The interrupt of the terminal: the job of the reader when it belongs to
 * one (a stage of a pipeline), the reader itself otherwise. */
static void tty_interrupt(void)
{
    struct task *self = task_self();
    k_u32_t job;

    if (self == NULL) {
        return;
    }

    job = task_get_job(self);

    if (job != TASK_JOB_NONE) {
        (void)task_signal_job(job, K_SIGINT);
        return;
    }

    (void)task_signal_send(self->pid, K_SIGINT);
}

/* The end of the line: the canonical reader has a line to hand out. */
static void tty_close_line(struct k_tty *tty)
{
    if (tty->cut) {
        /* The line was longer than the buffer: what is kept is the beginning,
         * and the rest of it is dropped to the newline (the rule of the shell
         * for a command that did not fit). */
        tty->cut = false;
        tty->length = 0;
        tty->position = 0;
        return;
    }

    tty->ready = true;
    tty->position = 0;
}

/*
 * One byte of the canonical mode: the editing of the line, the echo, and the
 * three characters that are a command rather than a byte.
 */
static int canon_one(struct k_tty *tty, int c)
{
    const bool echo = tty_echoes(tty);
    const bool isig = (tty->mode.c_lflag & K_TTY_ISIG) != 0;

    if (c == '\r' && (tty->mode.c_iflag & K_TTY_ICRNL) != 0) {
        c = '\n';
    }

    if (c == (int)tty->mode.c_cc[K_TTY_VINTR] && isig) {
        if (echo) {
            tty_echo("^C\r\n", 4);
        }
        tty->length = 0;
        tty->position = 0;
        tty->cut = false;
        tty_interrupt();
        return KERR_INTR;
    }

    if (c == (int)tty->mode.c_cc[K_TTY_VERASE] || c == '\b' || c == 127) {
        if (tty->length > 0) {
            tty->length--;
            if (echo) {
                tty_echo("\b \b", 3);
            }
        }
        return TTY_MORE;
    }

    if (c == (int)tty->mode.c_cc[K_TTY_VKILL]) {
        while (tty->length > 0) {
            tty->length--;
            if (echo) {
                tty_echo("\b \b", 3);
            }
        }
        tty->cut = false;
        return TTY_MORE;
    }

    if (c == (int)tty->mode.c_cc[K_TTY_VEOF]) {
        if (tty->length == 0) {
            if (echo) {
                tty_echo("^D", 2);
            }
            tty->eof = true;
            return TTY_EOF;
        }
        /* Ctrl+D in the middle of a line ends the line there. */
        tty_close_line(tty);
        return TTY_READY;
    }

    if (c == '\n') {
        if (echo) {
            tty_echo("\r\n", 2);
        }
        if (!tty->cut && tty->length + 1 < K_TTY_LINE_MAX) {
            tty->line[tty->length++] = '\n';
        }
        tty_close_line(tty);
        return TTY_MORE;
    }

    if (tty->length + 1 < K_TTY_LINE_MAX) {
        tty->line[tty->length++] = (char)c;
        if (echo) {
            char out = (char)c;

            tty_echo(&out, 1);
        }
    } else {
        tty->cut = true;
    }

    return TTY_MORE;
}

/* The bytes of a complete line that are already there, or -1 when the line is
 * not complete yet. A line is handed out as a whole: the bytes of a line that
 * a person is still typing are not an answer of the call. */
static int line_take(struct k_tty *tty, char *buf, k_size_t size)
{
    k_size_t count;

    if (!tty->ready || tty->position >= tty->length) {
        return -1;
    }

    count = tty->length - tty->position;
    if (count > size) {
        count = size;
    }

    memcpy(buf, tty->line + tty->position, count);
    tty->position += (k_u32_t)count;

    if (tty->position >= tty->length) {
        tty->length = 0;
        tty->position = 0;
        tty->ready = false;
    }

    return (int)count;
}

static int canon_read(struct k_tty *tty, char *buf, k_size_t size)
{
    for (;;) {
        int taken = line_take(tty, buf, size);
        int c;

        if (taken >= 0) {
            return taken;
        }

        if (tty->eof) {
            tty->eof = false;
            return 0;
        }

        c = tty_wait_byte(tty, K_WAIT_INFINITE);

        if (c == TTY_GONE) {
            return 0; /* the end of the file of the terminal */
        }
        if (c == TTY_SIGNAL) {
            return KERR_INTR;
        }
        if (c == TTY_NONE) {
            continue;
        }
        if (canon_one(tty, c) < 0) {
            return KERR_INTR;
        }
    }
}

static int raw_read(struct k_tty *tty, char *buf, k_size_t size)
{
    k_u32_t vmin = tty->mode.c_cc[K_TTY_VMIN];
    k_u32_t vtime = tty->mode.c_cc[K_TTY_VTIME];
    k_size_t count = 0;
    int c;

    if (vmin > size) {
        vmin = (k_u32_t)size;
    }

    /* The four combinations of POSIX: the two without a timer first, because
     * they are what every program of a screen asks for. */
    if (vmin > 0 && vtime == 0) {
        while (count < vmin) {
            c = tty_wait_byte(tty, K_WAIT_INFINITE);

            if (c == TTY_GONE) {
                return (int)count;
            }
            if (c == TTY_SIGNAL) {
                return (count > 0) ? (int)count : KERR_INTR;
            }
            if (c < 0) {
                continue;
            }

            buf[count++] = (char)c;
        }

        return (int)count;
    }

    if (vmin == 0 && vtime == 0) {
        c = tty_wait_byte(tty, 0);

        if (c == TTY_GONE || c < 0) {
            return 0;
        }

        buf[0] = (char)c;

        return 1;
    }

    if (vmin == 0) {
        c = tty_wait_byte(tty, vtime * 100u);

        if (c == TTY_GONE || c == TTY_NONE) {
            return 0;
        }
        if (c == TTY_SIGNAL) {
            return KERR_INTR;
        }

        buf[0] = (char)c;

        return 1;
    }

    /* VMIN bytes with a timer that starts at the first one. */
    c = tty_wait_byte(tty, K_WAIT_INFINITE);

    if (c == TTY_GONE) {
        return 0;
    }
    if (c == TTY_SIGNAL) {
        return KERR_INTR;
    }
    if (c < 0) {
        return 0;
    }

    buf[count++] = (char)c;

    {
        k_u64_t deadline = k_sched_uptime_ms() + (k_u64_t)vtime * 100u;

        while (count < vmin) {
            k_u64_t now = k_sched_uptime_ms();
            k_u32_t wait;

            if (now >= deadline) {
                break;
            }

            wait = (k_u32_t)(deadline - now);
            c = tty_wait_byte(tty, wait);

            if (c == TTY_GONE || c == TTY_NONE) {
                break;
            }
            if (c == TTY_SIGNAL) {
                break;
            }
            if (c < 0) {
                continue;
            }

            buf[count++] = (char)c;
        }
    }

    return (int)count;
}

int k_tty_read(char *buf, k_size_t size)
{
    struct k_tty *tty;
    struct task *self;
    int rc;

    if (buf == NULL || size == 0) {
        return 0;
    }

    tty = tty_current();
    if (tty == NULL) {
        return KERR_NOMEM;
    }

    /*
     * The keys belong to the task that reads them until it goes away: the
     * caller of execve watches the console for Ctrl+C, and an editor that
     * reads a key must not find it eaten by the task that waits for it.
     */
    self = task_self();
    if (self != NULL && tty == &g_console) {
        tty->owner = self->pid;
    }

    rc = tty_is_raw(tty) ? raw_read(tty, buf, size) : canon_read(tty, buf, size);

    return rc;
}

/* --- ioctl --- */

/*
 * The pointer of a request is the memory of the program, in the direction the
 * request travels: a `struct termios` is written by `TCGETS` and read by
 * `TCSETS`, and the kernel must not fault on either (kernel/port/exec.h).
 */
static bool user_range(k_u64_t address, k_size_t size)
{
    return k_port_exec_contains((const void *)(uintptr_t)address, size);
}

static int tty_get_termios(struct k_tty *tty, k_u64_t arg)
{
    if (!user_range(arg, sizeof(tty->mode))) {
        return KERR_FAULT;
    }

    memcpy((void *)(uintptr_t)arg, &tty->mode, sizeof(tty->mode));

    return 0;
}

static int tty_set_termios(struct k_tty *tty, k_u64_t arg, bool flush)
{
    struct k_tty_termios mode;
    struct task *self = task_self();

    if (!user_range(arg, sizeof(mode))) {
        return KERR_FAULT;
    }

    memcpy(&mode, (const void *)(uintptr_t)arg, sizeof(mode));

    /*
     * The canonical line being typed does not survive a change of the mode:
     * the bytes it holds were meant for the discipline that was there. The
     * input of the console has no queue of its own to drop beyond that.
     */
    tty->length = 0;
    tty->position = 0;
    tty->ready = false;
    tty->cut = false;
    tty->eof = false;
    tty->mode = mode;

    if (flush) {
        tty->pushback = -1;
    }

    /* A program that puts the console in raw mode owns its keys before it
     * reads the first one. */
    if (self != NULL && tty == &g_console && tty_is_raw(tty)) {
        tty->owner = self->pid;
    }

    return 0;
}

static int tty_get_winsize(k_u64_t arg)
{
    struct k_tty_winsize size;
    struct task *self = task_self();

    if (!user_range(arg, sizeof(size))) {
        return KERR_FAULT;
    }

    memset(&size, 0, sizeof(size));

    if (self != NULL && self->terminal != NULL) {
        /* A terminal of telnet has no negotiated size: the one of the
         * terminals of this system (docs/debts.md). */
        size.ws_row = 25;
        size.ws_col = 80;
    } else {
        k_port_console_size(&size.ws_row, &size.ws_col);
    }

    memcpy((void *)(uintptr_t)arg, &size, sizeof(size));

    return 0;
}

int k_tty_ioctl(k_u32_t request, k_u64_t arg)
{
    struct k_tty *tty = tty_current();

    if (tty == NULL) {
        return KERR_NOMEM;
    }

    switch (request) {
    case K_TTY_TCGETS:
        return tty_get_termios(tty, arg);
    case K_TTY_TCSETS:
    case K_TTY_TCSETSW:
        return tty_set_termios(tty, arg, false);
    case K_TTY_TCSETSF:
        return tty_set_termios(tty, arg, true);
    case K_TTY_TIOCGWINSZ:
        return tty_get_winsize(arg);
    case K_TTY_TIOCSWINSZ:
        /* A program may set a size and the machine answers its own: nothing
         * resizes a screen from a program (ADR 0051 §6). */
        return 0;
    case K_TTY_FIONREAD: {
        k_i32_t count = 0;

        if (!user_range(arg, sizeof(count))) {
            return KERR_FAULT;
        }

        if (tty->pushback >= 0) {
            count = 1;
        }
        if (tty->length > tty->position) {
            count += (k_i32_t)(tty->length - tty->position);
        }

        memcpy((void *)(uintptr_t)arg, &count, sizeof(count));
        return 0;
    }
    default:
        return KERR_INVAL;
    }
}

/* --- the questions of the rest of the kernel --- */

bool k_tty_console_claimed(void)
{
    return g_console.owner != 0;
}

bool k_tty_input_ready(void)
{
    struct k_tty *tty = tty_current();
    struct task *self = task_self();
    int c;

    if (tty == NULL) {
        return false;
    }

    if (tty->pushback >= 0 || tty->length > tty->position) {
        return true;
    }

    /* A terminal of telnet cannot be asked whether a byte waits without
     * waiting for one: the call that owns it answers "not yet" and the read
     * is the wait (docs/debts.md). */
    if (self != NULL && self->terminal != NULL) {
        return false;
    }

    c = k_port_console_poll();
    if (c < 0) {
        return false;
    }

    /* The byte is kept for the read and the console is the reader's from now
     * on: a poll is the first half of a read. */
    tty->pushback = c;
    if (self != NULL && tty == &g_console) {
        tty->owner = self->pid;
    }

    return true;
}

bool k_tty_console_raw(void)
{
    return tty_is_raw(&g_console);
}

void k_tty_task_exit(struct task *t)
{
    if (t == NULL) {
        return;
    }

    if (t->tty_state != NULL) {
        kmm_free(t->tty_state);
        t->tty_state = NULL;
    }

    /*
     * A program that dies in raw mode must not leave the keyboard of the
     * machine raw: the console goes back to the mode a terminal of this
     * system starts in, and its claim is released with it.
     */
    if (g_console.owner == t->pid) {
        tty_init(&g_console);
    }
}

void k_tty_init(void)
{
    tty_init(&g_console);
}
