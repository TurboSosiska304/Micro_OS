/*
 * Micro OS — the terminal of a program (ADR 0051).
 *
 * The terminal is checked where it is portable: a task of the tests is given
 * a `struct k_terminal` whose `get` answers a script of bytes, and the line
 * discipline, the raw mode, the control characters and the `ioctl` of the
 * terminal are read back through the calls a program uses - the kernel's
 * `k_tty_read`, and `ksys_call(KSYS_IOCTL, ...)` on a descriptor that is not
 * open, which is the rule of the console of 0, 1 and 2 (ADR 0051 §2).
 *
 * The mode of a task that has a terminal of its own belongs to the task and
 * not to the console, so a raw mode here is not the keyboard of the machine:
 * the test is the discipline and not the hardware.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "kernel/console.h"
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/tty.h"

/* The script of a terminal of the test: the bytes the person "types" and the
 * position of the next one. The end of the script is the end of the input. */
struct script {
    const char *bytes;
    k_size_t length;
    k_size_t position;
    k_u32_t delay_ms; /* a terminal that answers the end of the input late */
};

static struct script g_script;
static struct k_terminal g_terminal;

static int script_get(void *ctx)
{
    struct script *script = (struct script *)ctx;

    if (script->position >= script->length) {
        /* A terminal that is not gone yet: it waits and then says so, which is
         * what a read with a timer has to survive. */
        if (script->delay_ms > 0) {
            k_sched_sleep_ms(script->delay_ms);
        }
        return -1;
    }

    return (unsigned char)script->bytes[script->position++];
}

static void script_put(void *ctx, const char *data, k_size_t len)
{
    K_UNUSED(ctx);
    K_UNUSED(data);
    K_UNUSED(len);
}

static bool script_echo(void *ctx)
{
    K_UNUSED(ctx);

    return false; /* a terminal of telnet without the echo of the server */
}

static void use_script(const char *bytes, k_size_t length)
{
    g_script.bytes = bytes;
    g_script.length = length;
    g_script.position = 0;
    g_script.delay_ms = 0;

    g_terminal.ctx = &g_script;
    g_terminal.get = script_get;
    g_terminal.put = script_put;
    g_terminal.echo = script_echo;

    k_terminal_set(&g_terminal);
}

static void use_text(const char *text)
{
    use_script(text, strlen(text));
}

static void drop_terminal(void)
{
    k_terminal_set(NULL);
}

/* The defaults of a terminal of a Unix, and the raw mode of `cfmakeraw`. */
static void test_tty_defaults(void)
{
    struct k_tty_termios settings;
    struct k_tty_termios raw;

    ktest_begin("tty: a terminal starts in canonical mode with the echo");

    k_tty_defaults(&settings);
    KT_CHECK((settings.c_lflag & K_TTY_ICANON) != 0);
    KT_CHECK((settings.c_lflag & K_TTY_ECHO) != 0);
    KT_CHECK((settings.c_lflag & K_TTY_ISIG) != 0);
    KT_CHECK((settings.c_iflag & K_TTY_ICRNL) != 0);
    KT_CHECK_INT_EQ(settings.c_cc[K_TTY_VMIN], 1);
    KT_CHECK_INT_EQ(settings.c_cc[K_TTY_VTIME], 0);
    KT_CHECK_INT_EQ(settings.c_cc[K_TTY_VINTR], 3);
    KT_CHECK_INT_EQ(settings.c_cc[K_TTY_VEOF], 4);
    KT_CHECK_INT_EQ(sizeof(settings), 36);

    ktest_begin("tty: the raw mode of a screen clears the discipline");
    raw = settings;
    k_tty_make_raw(&raw);
    KT_CHECK((raw.c_lflag & K_TTY_ICANON) == 0);
    KT_CHECK((raw.c_lflag & K_TTY_ECHO) == 0);
    KT_CHECK((raw.c_lflag & K_TTY_ISIG) == 0);
    KT_CHECK((raw.c_iflag & K_TTY_ICRNL) == 0);
    KT_CHECK((raw.c_oflag & K_TTY_OPOST) == 0);
    KT_CHECK((raw.c_cflag & K_TTY_CSIZE) == K_TTY_CS8);
    KT_CHECK_INT_EQ(raw.c_cc[K_TTY_VMIN], 1);
    KT_CHECK_INT_EQ(raw.c_cc[K_TTY_VTIME], 0);
}

/* A line at a time: the read of the canonical mode is one line, and the end
 * of the script is the end of the file. */
static void test_tty_canonical(void)
{
    char line[32];
    int count;

    ktest_begin("tty: a canonical read answers the line the person typed");

    use_text("hello\nworld\n");

    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, 6);
    KT_CHECK(count == 6 && memcmp(line, "hello\n", 6) == 0);

    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, 6);
    KT_CHECK(count == 6 && memcmp(line, "world\n", 6) == 0);

    /* The terminal is gone: the end of the file and a read of nothing. */
    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, 0);

    drop_terminal();
}

/* The editing of the line: the erase of a character, and a read that is
 * smaller than the line keeps the rest of it for the next call (POSIX). */
static void test_tty_editing(void)
{
    char line[32];
    int count;

    ktest_begin("tty: the backspace erases a character of the line");
    use_text("ab\bX\n");
    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, 3);
    KT_CHECK(count == 3 && memcmp(line, "aX\n", 3) == 0);
    drop_terminal();

    ktest_begin("tty: a short read keeps the rest of the line");
    use_text("abcdef\n");
    count = k_tty_read(line, 3);
    KT_CHECK_INT_EQ(count, 3);
    KT_CHECK(count == 3 && memcmp(line, "abc", 3) == 0);
    count = k_tty_read(line, 3);
    KT_CHECK_INT_EQ(count, 3);
    KT_CHECK(count == 3 && memcmp(line, "def", 3) == 0);
    count = k_tty_read(line, 3);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK(count == 1 && line[0] == '\n');
    drop_terminal();
}

/* Ctrl+D: the end of the file at the start of a line, the end of the line in
 * the middle of one. */
static void test_tty_eof(void)
{
    char line[32];
    int count;

    ktest_begin("tty: Ctrl+D at the start of a line is the end of the file");
    use_text("ab\x04\x04");
    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, 2);
    KT_CHECK(count == 2 && memcmp(line, "ab", 2) == 0);
    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, 0);
    drop_terminal();
}

/* A line longer than the buffer is cut and dropped, the way the shell drops a
 * command that did not fit: a cut line is not the line that was typed. */
static void test_tty_long_line(void)
{
    char big[K_TTY_LINE_MAX + 72];
    char line[32];
    int count;
    k_size_t i;

    ktest_begin("tty: an overlong line is dropped to the newline");

    for (i = 0; i < sizeof(big) - 4; i++) {
        big[i] = 'x';
    }
    big[sizeof(big) - 4] = '\n';
    memcpy(&big[sizeof(big) - 3], "ok\n", 3);

    use_script(big, sizeof(big));
    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, 3); /* the long line is gone, the next one is not */
    KT_CHECK(count == 3 && memcmp(line, "ok\n", 3) == 0);
    drop_terminal();
}

/* The raw mode: VMIN 1 is a byte at a time, VMIN 0 with VTIME is a read that
 * returns after the tenths of a second it was given. */
static void test_tty_raw(void)
{
    struct k_tty_termios settings;
    struct k_tty_termios back;
    char byte;
    int count;
    k_u64_t started;
    k_u64_t elapsed;

    ktest_begin("tty: the raw mode of a terminal is a byte at a time");

    use_text("ab");
    k_tty_defaults(&settings);
    k_tty_make_raw(&settings);

    KT_CHECK_INT_EQ(
        ksys_call(KSYS_IOCTL, 0, K_TTY_TCSETS, (ksys_arg_t)(uintptr_t)&settings, 0, 0, 0), 0);

    /* The mode read back is the mode that was set: 36 bytes through the ioctl
     * and not a second copy of it. */
    memset(&back, 0, sizeof(back));
    KT_CHECK_INT_EQ(ksys_call(KSYS_IOCTL, 0, K_TTY_TCGETS, (ksys_arg_t)(uintptr_t)&back, 0, 0, 0),
                    0);
    KT_CHECK((back.c_lflag & K_TTY_ICANON) == 0);
    KT_CHECK((back.c_lflag & K_TTY_ECHO) == 0);
    KT_CHECK_INT_EQ(back.c_cc[K_TTY_VMIN], 1);

    count = k_tty_read(&byte, 1);
    KT_CHECK_INT_EQ(count, 1);
    KT_CHECK_INT_EQ(byte, 'a');

    /* VMIN 0, VTIME 1: the read returns zero after 100 ms of quiet. */
    settings.c_cc[K_TTY_VMIN] = 0;
    settings.c_cc[K_TTY_VTIME] = 1;
    KT_CHECK_INT_EQ(
        ksys_call(KSYS_IOCTL, 0, K_TTY_TCSETS, (ksys_arg_t)(uintptr_t)&settings, 0, 0, 0), 0);
    count = k_tty_read(&byte, 1);
    KT_CHECK_INT_EQ(count, 1); /* 'b' still waits */
    KT_CHECK_INT_EQ(byte, 'b');

    count = k_tty_read(&byte, 1); /* the script is exhausted: the terminal is gone */
    KT_CHECK_INT_EQ(count, 0);

    /* And a terminal with no byte at all answers zero instead of waiting for
     * ever: the read of an editor that draws the screen between two keys. */
    use_text("");
    g_script.delay_ms = 150;
    started = k_sched_uptime_ms();
    count = k_tty_read(&byte, 1);
    elapsed = k_sched_uptime_ms() - started;
    KT_CHECK_INT_EQ(count, 0);
    KT_CHECK(elapsed >= 90);

    drop_terminal();
}

/* The size of the terminal: the console of the machine answers its screen,
 * and the call is the one a program makes before it draws. */
static void test_tty_winsize(void)
{
    struct k_tty_winsize size;

    ktest_begin("tty: TIOCGWINSZ answers the size of the screen");

    memset(&size, 0, sizeof(size));
    KT_CHECK_INT_EQ(
        ksys_call(KSYS_IOCTL, 1, K_TTY_TIOCGWINSZ, (ksys_arg_t)(uintptr_t)&size, 0, 0, 0), 0);
    KT_CHECK(size.ws_row >= 24);
    KT_CHECK(size.ws_col >= 80);

    ktest_begin("tty: a request the terminal does not know is EINVAL");
    KT_CHECK_INT_EQ(ksys_call(KSYS_IOCTL, 0, 0x9999, 0, 0, 0, 0), KERR_INVAL);

    ktest_begin("tty: the console is canonical when no program holds it");
    KT_CHECK(!k_tty_console_raw());
}

static void tty_idle_task(void *arg)
{
    K_UNUSED(arg);
    for (;;) {
        k_sched_sleep_ms(1000);
    }
}

static void test_tty_pgrp_ctrlc(void)
{
    struct task *t1;
    struct task *t2;
    char line[16];
    int count;

    ktest_begin("tty: Ctrl+C sends SIGINT to the foreground process group");

    t1 = task_create("tty1", tty_idle_task, NULL, TASK_PRIO_DEFAULT);
    t2 = task_create("tty2", tty_idle_task, NULL, TASK_PRIO_DEFAULT);
    KT_CHECK(t1 != NULL);
    KT_CHECK(t2 != NULL);
    if (t1 == NULL || t2 == NULL) {
        return;
    }

    KT_CHECK_INT_EQ(task_setpgid(t1->pid, t1->pid), 0);
    KT_CHECK_INT_EQ(task_setpgid(t2->pid, t1->pid), 0);

    use_text("");
    k_tty_open();
    KT_CHECK_INT_EQ(k_tty_set_fg_pgrp(t1->pid), 0);
    k_tty_set_canonical();

    /* Ctrl+C interrupts the line and signals the foreground pgrp. */
    use_text("ab\x03"
             "cd\n");
    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, KERR_INTR);
    KT_CHECK((task_signal_pending(t1) & 1u) != 0);
    KT_CHECK((task_signal_pending(t2) & 1u) != 0);

    /* After interrupt the next line is readable. */
    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, 3);
    KT_CHECK(count == 3 && memcmp(line, "cd\n", 3) == 0);

    /* tcsetpgrp switches who gets the signal. */
    __atomic_fetch_and(&t1->sig_pending, ~1u, __ATOMIC_RELAXED);
    __atomic_fetch_and(&t2->sig_pending, ~1u, __ATOMIC_RELAXED);
    KT_CHECK_INT_EQ(task_setpgid(t2->pid, t2->pid), 0);
    KT_CHECK_INT_EQ(k_tty_set_fg_pgrp(t2->pid), 0);

    use_text("\x03");
    count = k_tty_read(line, sizeof(line));
    KT_CHECK_INT_EQ(count, KERR_INTR);
    KT_CHECK((task_signal_pending(t1) & 1u) == 0);
    KT_CHECK((task_signal_pending(t2) & 1u) != 0);

    k_tty_close();
    drop_terminal();
}

static void test_tty_session_release(void)
{
    k_u32_t pgid;

    ktest_begin("tty: closing the last descriptor releases the session");

    use_text("");
    k_tty_open();
    pgid = task_get_pgid(task_self());
    KT_CHECK_INT_EQ(k_tty_set_fg_pgrp(pgid), 0);
    KT_CHECK(k_tty_fg_pgrp() == pgid);
    k_tty_close();
    KT_CHECK_INT_EQ(k_tty_fg_pgrp(), 0);
    drop_terminal();
}

void test_tty(void)
{
    test_tty_defaults();
    test_tty_canonical();
    test_tty_editing();
    test_tty_eof();
    test_tty_long_line();
    test_tty_raw();
    test_tty_winsize();
    test_tty_pgrp_ctrlc();
    test_tty_session_release();
}
