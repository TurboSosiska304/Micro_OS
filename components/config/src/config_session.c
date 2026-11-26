/*
 * Micro OS — a session of the accounts: a terminal and the shell on it.
 *
 * See config/session.h for why this is a task of its own and why the init of
 * a port is its getty. The dialogue is deliberately the one of ADR 0033 with
 * one difference that is the point of the half of 1.8: the prompt, the echo
 * and the password go to the terminal of the task (kernel/console.h) and not
 * to the console of the machine, so a `login` typed over telnet is answered
 * where it was typed.
 *
 * The password of a session of the network is the hash of /etc/shadow and
 * nothing else is added: no new primitive, no new file, and the sentence of a
 * refusal does not say whether the name exists, because a list of the
 * accounts of a machine is the first thing a stranger asks for.
 */
#include "config/session.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel/errno.h"
#include "kernel/kmm.h"
#include "kernel/log.h"
#include "kernel/power.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/tty.h"
#include "kernel/version.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"

/* A name that is not in /etc/passwd is hashed against this record anyway, so
 * that the wait of a check is the wait of a name that exists. The hash is the
 * same shape as a line of /etc/shadow and matches no password of this tree. */
#define SESSION_DUMMY_HASH                                                                         \
    "$sha256$mox1$da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c"

/* The console session, as the init of a port knows it. Only init writes the
 * pid, and only from its own loop. */
static k_u32_t g_console_pid;

void session_write(const char *text)
{
    if (text == NULL) {
        return;
    }

    k_console_terminal_put(text, strlen(text));
}

void session_write_fmt(const char *fmt, ...)
{
    char buf[192];
    va_list ap;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    buf[sizeof(buf) - 1] = '\0';
    session_write(buf);
}

bool session_read_line(const char *prompt, char *buf, k_size_t size, bool secret, bool *too_long)
{
    k_size_t len = 0;
    bool cut = false;
    bool result;

    if (buf == NULL || size == 0) {
        return false;
    }

    if (too_long != NULL) {
        *too_long = false;
    }

    if (prompt != NULL && prompt[0] != '\0') {
        session_write(prompt);
    }

    if (!secret) {
        /* Normal lines go through the tty: line discipline, echo and Ctrl+C
         * delivery to the foreground process group live there. */
        k_tty_set_canonical();
        for (;;) {
            char tmp[SESSION_LINE_MAX];
            int got = k_tty_read(tmp, sizeof(tmp) - 1u);

            if (got == KERR_INTR) {
                /* Ctrl+C: the tty already echoed ^C and sent SIGINT to the
                 * foreground group. Clear a pending SIGINT on the reader so
                 * the session itself is not killed by its own prompt. */
                struct task *self = task_self();
                if (self != NULL) {
                    task_signal_fn old = task_signal_set(self, K_SIGINT, K_SIG_IGN);
                    task_signal_set(self, K_SIGINT, old);
                }
                if (too_long != NULL) {
                    *too_long = false;
                }
                buf[0] = '\0';
                return true;
            }

            if (got <= 0) {
                buf[0] = '\0';
                session_write("\n");
                return false;
            }

            len = 0;
            cut = false;
            for (int i = 0; i < got; i++) {
                char c = tmp[i];
                if (c == '\n' || c == '\r') {
                    break;
                }
                if (len + 1 < size) {
                    buf[len++] = c;
                } else {
                    cut = true;
                }
            }
            buf[len] = '\0';
            if (too_long != NULL) {
                *too_long = cut;
            }
            return true;
        }
    }

    /* The prompt is ordinary output and belongs in the log. From here until
     * the line ends, even an echo made by the port is hidden from run.log. */
    if (secret) {
        k_console_set_input_not_logged(true);
    }

    for (;;) {
        int c = k_console_getchar();

        if (c < 0) {
            /* The terminal is gone: the end of a file of the host, a client
             * of telnet that closed, a board told off. Not a wrong answer. */
            buf[len] = '\0';
            session_write("\n");
            result = false;
            goto done;
        }

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            if (cut) {
                if (too_long != NULL) {
                    *too_long = true;
                }
            }
            result = true;
            goto done;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
            }
            continue;
        }

        if (c == 3) { /* Ctrl+C: the line is dropped */
            len = 0;
            cut = false;
            continue;
        }

        if (len + 1 < size) {
            buf[len++] = (char)c;
        } else {
            /* A cut line is a different line: it is not executed. */
            cut = true;
        }
    }

done:
    if (secret) {
        k_console_set_input_not_logged(false);
    }
    return result;
}

static void read_account_name(char *name, k_size_t size)
{
    for (;;) {
        bool too_long = false;

        if (!session_read_line("login: ", name, size, false, &too_long)) {
            name[0] = '\0';
            return;
        }

        if (too_long) {
            session_write("login: the name does not fit\n");
            continue;
        }

        if (name[0] == '\0') {
            /* Enter is a person who typed nothing, and not a name that does
             * not fit; the shell says what it wanted and asks again. */
            session_write("login: a name is needed\n");
            continue;
        }

        return;
    }
}

int session_authenticate(const char *given, struct user_account *out, k_u32_t delay_ms)
{
    char name[USERS_NAME_MAX];
    char password[SESSION_LINE_MAX];
    struct user_account account;
    int attempt;
    int rc = USERS_EAUTH;

    if (out == NULL) {
        return USERS_EIO;
    }

    if (given != NULL) {
        if (strlen(given) + 1 > sizeof(name)) {
            session_write("login: the name does not fit\n");
            return USERS_ENOENT;
        }
        memcpy(name, given, strlen(given) + 1);
    } else {
        read_account_name(name, sizeof(name));
        if (name[0] == '\0') {
            return USERS_EAUTH;
        }
    }

    for (attempt = 0; attempt < SESSION_ATTEMPTS; attempt++) {
        bool too_long = false;

        if (!session_read_line("password: ", password, sizeof(password), true, &too_long)) {
            memset(password, 0, sizeof(password));
            return USERS_EAUTH;
        }

        rc = users_authenticate(name, password, &account);

        /*
         * A name that does not exist costs the same hash as one that does:
         * the digest of the nonsense above has no password, and the answer
         * of the two is the same sentence after the same work.
         */
        if (rc == USERS_ENOENT) {
            (void)users_hash_check(SESSION_DUMMY_HASH, password);
            rc = USERS_EAUTH;
        }

        memset(password, 0, sizeof(password));

        if (rc == USERS_OK) {
            *out = account;
            return USERS_OK;
        }

        if (rc != USERS_EAUTH) {
            /* A file that cannot be read is not a wrong password: no retry
             * of the person helps, and the sentence is the one of the file. */
            return rc;
        }

        if (delay_ms > 0) {
            k_sched_sleep_ms(delay_ms);
        }

        session_write("login: authentication failure\n");
    }

    return USERS_EAUTH;
}

/*
 * The commands of the console session before its prompt: the file
 * `/etc/session`, one line a command, the way the scripts of `/etc/init.d` are
 * commands of a task. The desktop of a machine is the one line `exec /bin/wm`:
 * the session of the console starts it, the shell waits for it, and the prompt
 * of the shell comes back when the last window of the desktop closes (ADR
 * 0054). A machine without the file is the machine of 1.7 and boots to the
 * prompt, which is what the smoke tests of the shell and of `/bin` read.
 *
 * The script runs in this task and not in a task of its own: the desktop is a
 * foreground program of the session that started it, and a `logout` among its
 * lines ends the session like a typed one. A session of telnet does not read
 * the file - the console of the machine is not the terminal of a client.
 */
#define SESSION_SCRIPT "/etc/session"
#define SESSION_SCRIPT_MAX 512

static bool session_startup(void)
{
    struct vfs_file file;
    k_u8_t priv[VFS_FILE_DATA_SIZE];
    char text[SESSION_SCRIPT_MAX + 1];
    char line[SESSION_LINE_MAX];
    int got;
    int rc;
    k_size_t at = 0;

    rc = vfs_open(SESSION_SCRIPT, VFS_O_RDONLY, &file, priv);
    if (rc != 0) {
        return false;
    }

    got = vfs_read(&file, text, SESSION_SCRIPT_MAX);
    (void)vfs_close(&file);

    if (got <= 0) {
        return false;
    }

    text[got] = '\0';
    kinfo("session: the commands of %s are running", SESSION_SCRIPT);

    while (text[at] != '\0') {
        k_size_t length = 0;
        bool cut = false;

        while (text[at] != '\0' && text[at] != '\n' && length + 1 < sizeof(line)) {
            line[length++] = text[at++];
        }

        /* The rest of a line that did not fit is thrown away and the line is
         * not executed: half a command is a different command (the rule of the
         * prompt of the shell). */
        if (text[at] != '\0' && text[at] != '\n') {
            cut = true;

            while (text[at] != '\0' && text[at] != '\n') {
                at++;
            }
        }

        if (text[at] == '\n') {
            at++;
        }

        while (length > 0 &&
               (line[length - 1] == ' ' || line[length - 1] == '\t' || line[length - 1] == '\r')) {
            length--;
        }

        while (length > 0 && (line[0] == ' ' || line[0] == '\t')) {
            memmove(line, line + 1, length - 1u);
            length--;
        }

        line[length] = '\0';

        if (length == 0 || line[0] == '#' || cut) {
            continue;
        }

        kinfo("session: %s", line);

        if (kshell_execute(line) == KSHELL_LOGOUT) {
            return true;
        }
    }

    return false;
}

int session_run(void)
{
    char line[SESSION_LINE_MAX];
    struct task *self = task_self();
    k_u8_t tty = task_get_tty(self);

    if (tty == TASK_TTY_CONSOLE) {
        session_write_fmt("%s shell ready. Type 'help' for the list of commands.\n", kernel_name());

        if (session_startup()) {
            return SESSION_EXIT_LOGOUT;
        }
    }

    for (;;) {
        bool too_long = false;
        int status;

        session_write("microos> ");

        if (!session_read_line(NULL, line, sizeof(line), false, &too_long)) {
            return SESSION_EXIT_CLOSED;
        }

        if (too_long) {
            session_write("line too long, it was not executed\n");
            continue;
        }

        if (line[0] == '\0') {
            continue;
        }

        status = kshell_execute(line);
        if (status == KSHELL_LOGOUT) {
            return SESSION_EXIT_LOGOUT;
        }
    }
}

/*
 * The end of a session: the terminal is taken away first, so that the line
 * the kernel logs about the end of the task goes to the console of the
 * machine and not to a client that asked for a shell, and then the task ends
 * with the answer the init of the port reads.
 */
static void session_finish(int status)
{
    k_terminal_set(NULL);
    task_exit(status);
}

void session_task(void *arg)
{
    struct session_info *info = arg;
    struct k_terminal terminal;
    struct task *self = task_self();
    k_u8_t tty;

    if (info == NULL) {
        session_finish(SESSION_EXIT_DENIED);
    }

    terminal = info->terminal;
    tty = info->tty;
    kmm_free(info);

    if (self != NULL) {
        task_set_tty(self, tty);
    }

    /*
     * The console of the machine is the port: a session of it sets no
     * terminal, and every read and write of the dialogue falls through to
     * the console, exactly as it did before there were sessions.
     */
    if (terminal.get != NULL) {
        k_terminal_set(&terminal);
    }

    {
        struct vfs_file tty_file;
        k_u8_t tty_priv[VFS_FILE_DATA_SIZE];
        bool tty_opened = (vfs_open("/dev/tty", VFS_O_RDWR, &tty_file, tty_priv) == 0);

        if (self != NULL) {
            (void)k_tty_set_fg_pgrp(task_get_pgid(self));
        }

        if (tty == TASK_TTY_TELNET) {
            struct user_account account;
            int rc = session_authenticate(NULL, &account, SESSION_FAIL_DELAY_MS);

            if (rc != USERS_OK) {
                if (tty_opened) {
                    (void)vfs_close(&tty_file);
                }
                session_finish(SESSION_EXIT_DENIED);
            }

            rc = users_apply(&account, true);
            if (rc != USERS_OK) {
                if (tty_opened) {
                    (void)vfs_close(&tty_file);
                }
                session_finish(SESSION_EXIT_DENIED);
            }

            session_write_fmt("\nlogin: %s, uid %u, gid %u, home %s\n", account.name,
                              (unsigned)account.uid, (unsigned)account.gid,
                              (account.home[0] != '\0') ? account.home : "/");
        }

        {
            int status = session_run();
            if (tty_opened) {
                (void)vfs_close(&tty_file);
            }
            session_finish(status);
        }
    }
}

k_u32_t session_start(const struct session_info *info)
{
    struct session_info *copy;
    struct task *task;
    const char *name;

    if (info == NULL) {
        return 0;
    }

    /*
     * A machine that is going down does not open a new door (ADR 0050):
     * the getty of every port reads the same flag, so the console, telnet
     * and a shell that starts a session of its own all stop here.
     */
    if (k_power_ending()) {
        kwarn("session: the machine is going down, no new session");
        return 0;
    }

    copy = kmm_alloc(sizeof(*copy), KMM_PRIORITY_SYSTEM);
    if (copy == NULL) {
        kerror("session: no memory for the record of a session");
        return 0;
    }

    *copy = *info;
    name = (info->tty == TASK_TTY_TELNET) ? "telnets" : "shell";

    task = task_create(name, session_task, copy, TASK_PRIO_DEFAULT);
    if (task == NULL) {
        kmm_free(copy);
        return 0;
    }

    return task->pid;
}

k_u32_t session_console_start(void)
{
    struct session_info info;

    memset(&info, 0, sizeof(info));
    info.tty = TASK_TTY_CONSOLE;

    g_console_pid = session_start(&info);
    if (g_console_pid == 0) {
        kerror("session: the console has no session");
    }

    return g_console_pid;
}

void session_console_reaped(k_u32_t pid, int status)
{
    if (pid == 0 || pid != g_console_pid) {
        return;
    }

    g_console_pid = 0;

    /*
     * A logout during the clean end is not a door to open again: the
     * machine is going down and the getty stays quiet (ADR 0050).
     */
    if (k_power_ending()) {
        return;
    }

    /*
     * A logout is a session that ended for a person who wants the door
     * again, and the new session starts as root because the init that makes
     * it is root: that is the way out of a session of a user, which cannot
     * climb back with one credential (ADR 0033).
     */
    if (status == SESSION_EXIT_LOGOUT) {
        (void)session_console_start();
    }
}

/* --- the commands ------------------------------------------------------- */

static const char *account_name(k_u32_t uid, char *buf, k_size_t size)
{
    struct user_account account;

    if (users_find_uid(uid, &account) == USERS_OK) {
        (void)snprintf(buf, size, "%s", account.name);
    } else {
        (void)snprintf(buf, size, "%u", (unsigned)uid);
    }

    return buf;
}

static int cmd_logout(int argc, char **argv)
{
    struct task *self = task_self();

    (void)argv;

    if (argc != 1) {
        kshell_printf("usage: logout\n");
        return KSHELL_USAGE;
    }

    if (self == NULL || task_get_tty(self) == TASK_TTY_NONE) {
        kshell_printf("logout: this task is not a session\n");
        return KSHELL_ERROR;
    }

    kshell_printf("logout: the session ends\n");

    return KSHELL_LOGOUT;
}

static int cmd_who(int argc, char **argv)
{
    k_size_t count;
    k_size_t i;

    (void)argv;

    if (argc != 1) {
        kshell_printf("usage: who\n");
        return KSHELL_USAGE;
    }

    kshell_printf("%-15s %-8s %s\n", "user", "tty", "pid");

    count = task_count();
    for (i = 0; i < count + 8; i++) {
        struct task *task = task_get_by_index(i);
        char name[USERS_NAME_MAX];
        k_u8_t tty;

        if (task == NULL) {
            continue;
        }

        tty = task_get_tty(task);
        if (tty == TASK_TTY_NONE) {
            continue;
        }

        kshell_printf("%-15s %-8s %u\n", account_name(task_get_uid(task), name, sizeof(name)),
                      task_tty_name(tty), (unsigned)task->pid);
    }

    return KSHELL_OK;
}

static int cmd_whoami(int argc, char **argv)
{
    struct task *self = task_self();
    char name[USERS_NAME_MAX];

    (void)argv;

    if (argc != 1) {
        kshell_printf("usage: whoami\n");
        return KSHELL_USAGE;
    }

    if (self == NULL) {
        return KSHELL_ERROR;
    }

    kshell_printf("%s\n", account_name(task_get_uid(self), name, sizeof(name)));

    return KSHELL_OK;
}

int session_register(void)
{
    static const struct kshell_help logout_help = {
        .summary = "end the session of this terminal",
        .usage = "logout",
        .detail = "The session ends and its task leaves the terminal. On the console the\n"
                  "init of the machine starts a new session as root, which is the way back\n"
                  "from a user of this system; a client of telnet is disconnected and the\n"
                  "next connection is asked for a name and a password again.\n",
    };
    static const struct kshell_help who_help = {
        .summary = "the sessions that are in",
        .usage = "who",
        .detail = "The name of /etc/passwd, the terminal and the pid of every session of\n"
                  "the machine: the console and the clients of telnet (/proc/sessions).\n",
    };
    static const struct kshell_help whoami_help = {
        .summary = "the name of the user of this shell",
        .usage = "whoami",
        .detail = "The name of the account of /etc/passwd whose uid the shell runs with.\n",
    };
    int rc = 0;

    rc |= kshell_register_help("logout", &logout_help, cmd_logout);
    rc |= kshell_register_help("who", &who_help, cmd_who);
    rc |= kshell_register_help("whoami", &whoami_help, cmd_whoami);

    return rc;
}
