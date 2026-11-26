/*
 * Micro OS — a session: a terminal, a door and the lines of a shell.
 *
 * The properties: a line of a dialogue comes from the terminal of the task
 * and not from the console of the machine, a secret line is never echoed, a
 * client that asked for no echo gets none, a closed terminal is not a wrong
 * answer, the door takes the right password and says the same sentence for a
 * wrong one and for a name that does not exist, and `logout` ends the loop.
 *
 * The terminal here is a script of bytes and a buffer, which is the same
 * contract a client of telnet fills with a socket (components/telnet). The
 * runner of the tests is a kernel task, so it can take a terminal the way a
 * session does.
 */
#include "ktest.h"
#include "tests/tests.h"

#include <string.h>

#include "config/session.h"
#include "config/users.h"
#include "kernel/ksys.h"
#include "kernel/sched.h"
#include "kernel/task.h"
#include "kernel/vfs.h"
#include "kshell/kshell.h"
#include "mfs/mfs.h"
#include "mfs/mfs_ram_device.h"
#include "mfs/mfs_vfs.h"

/* A terminal of a script: the bytes a person types and the bytes the server
 * wrote. `echo` is the answer of the negotiation of a client of telnet. */
struct fake_terminal {
    struct k_terminal io;
    const char *input;
    size_t at;
    char out[512];
    size_t out_len;
    bool echo;
    bool saw_input_hidden;
    bool saw_input_logged;
};

static struct mfs_ram_device g_volume;
static struct k_blockdev g_device;

static void setup(void)
{
    if (mfs_ram_init(&g_volume, 256) != 0) {
        return;
    }

    mfs_ram_device(&g_volume, &g_device);
    mfs_format(&g_device, 32);
    mfs_mount(&g_device);
    mfs_vfs_register("/");
    ksys_init();
}

static void teardown(void)
{
    k_terminal_set(NULL);

    vfs_umount("/");
    mfs_unmount();
    mfs_ram_free(&g_volume);
}

static void write_text(const char *path, const char *text)
{
    int fd = ksys_open(path, KSYS_O_WRONLY | KSYS_O_CREAT | KSYS_O_TRUNC);

    KT_CHECK(fd >= 0);
    if (fd >= 0) {
        KT_CHECK_INT_EQ(ksys_write(fd, text, strlen(text)), (int)strlen(text));
        KT_CHECK_INT_EQ(ksys_close(fd), 0);
    }
}

static int fake_get(void *arg)
{
    struct fake_terminal *terminal = arg;

    if (terminal->at >= strlen(terminal->input)) {
        return -1;
    }

    if (k_console_input_not_logged()) {
        terminal->saw_input_hidden = true;
    } else {
        terminal->saw_input_logged = true;
    }

    return (int)(unsigned char)terminal->input[terminal->at++];
}

static void fake_put(void *arg, const char *data, k_size_t len)
{
    struct fake_terminal *terminal = arg;

    if (terminal->out_len + len >= sizeof(terminal->out)) {
        len = sizeof(terminal->out) - 1 - terminal->out_len;
    }

    memcpy(terminal->out + terminal->out_len, data, len);
    terminal->out_len += len;
    terminal->out[terminal->out_len] = '\0';
}

static bool fake_echo(void *arg)
{
    struct fake_terminal *terminal = arg;

    return terminal->echo;
}

static void install(struct fake_terminal *terminal, const char *input, bool echo)
{
    memset(terminal, 0, sizeof(*terminal));
    terminal->input = input;
    terminal->echo = echo;
    terminal->io.ctx = terminal;
    terminal->io.get = fake_get;
    terminal->io.put = fake_put;
    terminal->io.echo = fake_echo;

    k_terminal_set(&terminal->io);
}

/* Does the text appear in what the server wrote? */
static bool wrote(const struct fake_terminal *terminal, const char *text)
{
    return strstr(terminal->out, text) != NULL;
}

static size_t count_of(const struct fake_terminal *terminal, const char *text)
{
    size_t count = 0;
    const char *at = terminal->out;

    while ((at = strstr(at, text)) != NULL) {
        count++;
        at += strlen(text);
    }

    return count;
}

/*
 * The property: the prompt and the echo go to the terminal of the task, and a
 * secret line is read without a byte of it repeated.
 */
static void test_read_line(void)
{
    struct fake_terminal terminal;
    char line[64];
    bool too_long = false;

    ktest_begin("session: a password is read from the terminal and not echoed");

    install(&terminal, "microos\n", true);

    KT_CHECK(session_read_line("password: ", line, sizeof(line), true, &too_long));
    KT_CHECK_STR_EQ(line, "microos");
    KT_CHECK(!too_long);
    KT_CHECK(wrote(&terminal, "password: "));
    KT_CHECK(!wrote(&terminal, "microos"));
    KT_CHECK_STR_EQ(terminal.out, "password: ");
    KT_CHECK(terminal.saw_input_hidden);
    KT_CHECK(!terminal.saw_input_logged);
    KT_CHECK(!k_console_input_not_logged());

    k_terminal_set(NULL);

    /* A line that is not secret is repeated, and backspace erases a byte on
     * the screen as it erases it from the line. */
    install(&terminal, "abX\b\n", true);
    KT_CHECK(session_read_line(NULL, line, sizeof(line), false, &too_long));
    KT_CHECK_STR_EQ(line, "ab");
    KT_CHECK(wrote(&terminal, "abX"));
    KT_CHECK(wrote(&terminal, "\b \b"));
    KT_CHECK(terminal.saw_input_logged);
    KT_CHECK(!terminal.saw_input_hidden);
    k_terminal_set(NULL);

    /* A client that kept its own echo gets no echo from the server. */
    install(&terminal, "hello\n", false);
    KT_CHECK(session_read_line(NULL, line, sizeof(line), false, &too_long));
    KT_CHECK_STR_EQ(line, "hello");
    KT_CHECK(terminal.out_len == 0);
    k_terminal_set(NULL);

    /* A line that does not fit is thrown away whole, not cut. */
    install(&terminal, "0123456789abcdef\n", true);
    KT_CHECK(session_read_line(NULL, line, 8, false, &too_long));
    KT_CHECK(too_long);
    KT_CHECK_STR_EQ(line, "0123456");
    k_terminal_set(NULL);

    /* The end of the input is a terminal that closed, and not a line. */
    install(&terminal, "", true);
    KT_CHECK(!session_read_line(NULL, line, sizeof(line), false, &too_long));
    k_terminal_set(NULL);

    /* A secret terminal that closes must not leave the task's log muted. */
    install(&terminal, "", true);
    KT_CHECK(!session_read_line(NULL, line, sizeof(line), true, &too_long));
    KT_CHECK(!k_console_input_not_logged());
    k_terminal_set(NULL);
}

/*
 * The property: the door takes the name and the password of the terminal, a
 * wrong answer costs the same sentence and a wait, and a name that does not
 * exist is not told apart from a password that is not the one.
 */
static void test_door(void)
{
    struct fake_terminal terminal;
    struct user_account account;
    k_u64_t started;
    k_u64_t elapsed;

    ktest_begin("session: the door of a session reads the terminal");

    setup();
    KT_CHECK_INT_EQ(ksys_mkdir("/etc"), 0);
    write_text(USERS_PASSWD_PATH, "root:0:0:/:/bin/sh\n"
                                  "guest:7:7:/home/guest:/bin/sh\n");
    write_text(USERS_SHADOW_PATH,
               "root:$sha256$mox1$"
               "da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c\n"
               "guest:*\n");

    /* The right name and the right password. */
    install(&terminal, "root\nmicroos\n", true);
    KT_CHECK_INT_EQ(session_authenticate(NULL, &account, 0), USERS_OK);
    KT_CHECK_STR_EQ(account.name, "root");
    KT_CHECK(wrote(&terminal, "login: "));
    KT_CHECK(wrote(&terminal, "password: "));
    KT_CHECK(!wrote(&terminal, "microos"));
    KT_CHECK(wrote(&terminal, "root\r\n") ||
             wrote(&terminal, "root\n")); /* the name is echoed, the answer not */
    k_terminal_set(NULL);

    /* A wrong password, three times: three sentences, one wait each, and no
     * byte of the password on the terminal. */
    install(&terminal, "root\nbad\nworse\nworst\n", true);
    started = k_sched_uptime_ms();
    KT_CHECK_INT_EQ(session_authenticate(NULL, &account, 20), USERS_EAUTH);
    elapsed = k_sched_uptime_ms() - started;
    KT_CHECK_INT_EQ((int)count_of(&terminal, "login: authentication failure"), 3);
    KT_CHECK(!wrote(&terminal, "bad"));
    KT_CHECK(elapsed >= 60);
    k_terminal_set(NULL);

    /* A name that does not exist is the same sentence, the same times: an
     * attacker does not learn which accounts this machine has. */
    install(&terminal, "nobody\nbad\nworse\nworst\n", true);
    KT_CHECK_INT_EQ(session_authenticate(NULL, &account, 0), USERS_EAUTH);
    KT_CHECK_INT_EQ((int)count_of(&terminal, "login: authentication failure"), 3);
    KT_CHECK(!wrote(&terminal, "no such account"));
    k_terminal_set(NULL);

    /* A locked account fails like a wrong password (guest has `*`). */
    install(&terminal, "guest\nx\nx\nx\n", true);
    KT_CHECK_INT_EQ(session_authenticate(NULL, &account, 0), USERS_EAUTH);
    k_terminal_set(NULL);

    /* The terminal closed before the password: no session, and no sentence
     * about the password at all. */
    install(&terminal, "root\n", true);
    KT_CHECK_INT_EQ(session_authenticate(NULL, &account, 0), USERS_EAUTH);
    KT_CHECK(!wrote(&terminal, "authentication failure"));
    k_terminal_set(NULL);

    teardown();
}

/*
 * The property: the loop of a session runs the lines of its terminal, the
 * commands of the accounts answer there, and `logout` leaves the loop.
 */
static void test_run(void)
{
    struct fake_terminal terminal;

    ktest_begin("session: logout ends the loop of a session");

    setup();
    KT_CHECK_INT_EQ(ksys_mkdir("/etc"), 0);
    write_text(USERS_PASSWD_PATH, "root:0:0:/:/bin/sh\n");
    write_text(USERS_SHADOW_PATH,
               "root:$sha256$mox1$"
               "da6e6da86105cbac14d7689b4fa29e184c12ef42347314cba33be87095daef2c\n");

    /* The commands of the shell have to exist: the runner calls the
     * registration the boot calls, and registering twice is the same table. */
    KT_CHECK_INT_EQ(kshell_init(), KSHELL_OK);
    KT_CHECK_INT_EQ(session_register(), KSHELL_OK);

    install(&terminal, "whoami\nlogout\n", true);
    task_set_tty(task_self(), TASK_TTY_TELNET);
    KT_CHECK_INT_EQ(session_run(), SESSION_EXIT_LOGOUT);
    KT_CHECK(wrote(&terminal, "microos> "));
    task_set_tty(task_self(), TASK_TTY_NONE);
    k_terminal_set(NULL);

    /* A terminal that closes ends the session as well, and it is not a
     * logout: nothing would start another one. */
    install(&terminal, "", true);
    KT_CHECK_INT_EQ(session_run(), SESSION_EXIT_CLOSED);
    k_terminal_set(NULL);

    teardown();
}

void test_session(void)
{
    test_read_line();
    test_door();
    test_run();
}
