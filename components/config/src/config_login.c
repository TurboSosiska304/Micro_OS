/*
 * Micro OS — `login`, `su` and `id`: the console and the accounts.
 *
 * The console of this machine is the shell, and the shell starts as root: the
 * board and the machine of QEMU have no getty, no `init` that reads a line
 * before the shell and no program of a console of its own. So `login` does not
 * start a shell — one is already running — it asks for the name and the
 * password, and hands the running shell to that user: the uid and the gid of
 * the task move, the home becomes its directory, and every program the person
 * starts from then on inherits the identity. The prompt does not change, and
 * the line after the login says who the shell is; `id` says it again at any
 * moment.
 *
 * `login` is the door of a console that has no session, so only root runs it:
 * a shell that became a user is already somebody's session, and the way to
 * another user — root included — is `su`, which asks for the password of the
 * account it becomes. A task has one credential and no saved uid (ADR 0005,
 * docs/syscall-abi.md), so the shell cannot climb back with `setuid`; it
 * climbs with `su` and the password of root, which is a privileged act of the
 * console and not a program of the disk. That is the price of one credential
 * and the reason the door is the console and not a setuid bit in an image
 * (ADR 0033).
 */
#include "config/users.h"

#include <string.h>

#include "kernel/console.h"
#include "kernel/log.h"
#include "kernel/task.h"
#include "kshell/kshell.h"

/* The attempts of a password at `login` before the door closes, and the
 * longest line a person types here. */
#define LOGIN_ATTEMPTS 3
#define LOGIN_LINE_MAX 64

/* The sentences of a refusal. They are one place, so that `login` and `su`
 * cannot say different things about the same file. */
static void say_error(const char *command, int rc)
{
    switch (rc) {
    case USERS_ENOENT:
        kshell_printf("%s: no such account\n", command);
        break;
    case USERS_EAUTH:
        kshell_printf("%s: authentication failure\n", command);
        break;
    case USERS_EFORMAT:
        kshell_printf("%s: /etc/passwd or /etc/shadow is not in the format of this system\n",
                      command);
        break;
    case USERS_EPERM:
        kshell_printf("%s: the credentials of the task cannot be changed\n", command);
        break;
    default:
        kshell_printf("%s: the file of accounts cannot be read\n", command);
        break;
    }
}

/*
 * A line of the console. The prompt is written, the characters are read one
 * at a time and echoed unless `secret` is true — a password is not repeated on
 * the screen and not in the log of a telnet client. False when the console is
 * closed (the end of the input), which is not a wrong answer.
 */
static bool console_line(const char *prompt, char *buf, k_size_t size, bool secret)
{
    k_size_t len = 0;

    kshell_printf("%s", prompt);

    for (;;) {
        int c = k_console_getchar();

        if (c < 0) {
            buf[len] = '\0';
            kshell_printf("\n");
            return false;
        }

        if (c == '\n' || c == '\r') {
            buf[len] = '\0';
            kshell_printf("\n");
            return true;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                if (!secret) {
                    k_console_write("\b \b", 3);
                }
            }
            continue;
        }

        if (c == 3) { /* Ctrl+C: the line is dropped and the door closes */
            buf[0] = '\0';
            kshell_printf("\n");
            return false;
        }

        if (len + 1 < size) {
            char typed = (char)c;

            buf[len++] = typed;
            if (!secret) {
                k_console_write(&typed, 1);
            }
        }
    }
}

/*
 * An answer of this file that is not one of the codes of config/users.h: the
 * person gave no name, the sentence is already on the console, and the caller
 * must not add another one.
 */
#define LOGIN_ECANCEL (-100)

/* A name from the command line or from the console; false when there is
 * none. */
static bool ask_name(char *name, k_size_t size)
{
    char line[LOGIN_LINE_MAX];

    if (!console_line("login: ", line, sizeof(line), false)) {
        return false;
    }

    if (line[0] == '\0') {
        /* A name of no letters is not a name that does not fit: the person
         * pressed Enter and the shell says what it wanted. */
        kshell_printf("login: a name is needed\n");
        return false;
    }

    if (strlen(line) + 1 > size) {
        kshell_printf("login: the name does not fit\n");
        return false;
    }

    memcpy(name, line, strlen(line) + 1);

    return true;
}

/*
 * The name and the password of a person, three times. The name is asked once
 * and the password again after a wrong one, as the console of a Unix-like
 * system does, and the third failure is the last.
 */
static int ask_credentials(const char *given, struct user_account *account)
{
    char name[USERS_NAME_MAX];
    char password[LOGIN_LINE_MAX];
    int attempt;
    int rc = USERS_EAUTH;

    if (given != NULL) {
        if (strlen(given) + 1 > sizeof(name)) {
            kshell_printf("login: the name does not fit\n");
            return USERS_ENOENT;
        }
        memcpy(name, given, strlen(given) + 1);
    } else if (!ask_name(name, sizeof(name))) {
        return LOGIN_ECANCEL;
    }

    for (attempt = 0; attempt < LOGIN_ATTEMPTS; attempt++) {
        if (!console_line("password: ", password, sizeof(password), true)) {
            return USERS_EAUTH;
        }

        rc = users_authenticate(name, password, account);
        if (rc == USERS_OK) {
            return USERS_OK;
        }
        if (rc != USERS_EAUTH) {
            return rc; /* a missing account or a broken file: no retry helps */
        }

        /* The last failure is said by the caller, so the person sees one
         * sentence per attempt and not two for the third. */
        if (attempt + 1 < LOGIN_ATTEMPTS) {
            kshell_printf("login: authentication failure\n");
        }
    }

    return USERS_EAUTH;
}

static int cmd_login(int argc, char **argv)
{
    struct task *self = task_self();
    struct user_account account;
    int rc;

    if (argc > 2) {
        kshell_printf("usage: login [name]\n");
        return KSHELL_USAGE;
    }

    if (self == NULL || !task_is_root(self)) {
        kshell_printf("login: this shell is already a session; `su` changes the user\n");
        return KSHELL_ERROR;
    }

    rc = ask_credentials((argc == 2) ? argv[1] : NULL, &account);
    if (rc != USERS_OK) {
        /* No name at all: the sentence of the prompt is the answer. */
        if (rc != LOGIN_ECANCEL) {
            say_error("login", rc);
        }
        return KSHELL_ERROR;
    }

    rc = users_apply(&account, true);
    if (rc != USERS_OK) {
        say_error("login", rc);
        return KSHELL_ERROR;
    }

    kshell_printf("login: %s, uid %u, gid %u, home %s\n", account.name, (unsigned)account.uid,
                  (unsigned)account.gid, (account.home[0] != '\0') ? account.home : "/");

    return KSHELL_OK;
}

static int cmd_su(int argc, char **argv)
{
    struct task *self = task_self();
    struct user_account account;
    char password[LOGIN_LINE_MAX];
    const char *name = "root";
    bool home = false;
    int rc;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0) {
            home = true;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            kshell_printf("usage: su [-|-l] [name]\n");
            return KSHELL_USAGE;
        } else {
            name = argv[i];
        }
    }

    rc = users_find_name(name, &account);
    if (rc != USERS_OK) {
        say_error("su", rc);
        return KSHELL_ERROR;
    }

    /*
     * Root becomes anybody without a password: root is already the person who
     * could read /etc/shadow and give itself any name. Everybody else has to
     * know the password of the account it becomes — its own included, unless
     * it already is that person.
     */
    if (self == NULL) {
        return KSHELL_ERROR;
    }

    if (!task_is_root(self) && task_get_uid(self) != account.uid) {
        if (!console_line("password: ", password, sizeof(password), true)) {
            return KSHELL_ERROR;
        }

        if (users_authenticate(account.name, password, &account) != USERS_OK) {
            kshell_printf("su: authentication failure\n");
            return KSHELL_ERROR;
        }
    }

    rc = users_apply(&account, home);
    if (rc != USERS_OK) {
        say_error("su", rc);
        return KSHELL_ERROR;
    }

    return KSHELL_OK;
}

static int cmd_id(int argc, char **argv)
{
    struct task *self = task_self();
    struct user_account account;
    k_u32_t uid;
    k_u32_t gid;

    (void)argv;

    if (argc != 1) {
        kshell_printf("usage: id\n");
        return KSHELL_USAGE;
    }

    if (self == NULL) {
        return KSHELL_ERROR;
    }

    uid = task_get_uid(self);
    gid = task_get_gid(self);

    if (users_find_uid(uid, &account) == USERS_OK) {
        kshell_printf("uid=%u(%s) gid=%u home=%s shell=%s\n", (unsigned)uid, account.name,
                      (unsigned)gid, (account.home[0] != '\0') ? account.home : "/",
                      (account.shell[0] != '\0') ? account.shell : "none");
    } else {
        kshell_printf("uid=%u gid=%u (no account of this uid)\n", (unsigned)uid, (unsigned)gid);
    }

    return KSHELL_OK;
}

int users_register(void)
{
    static const struct kshell_help login_help = {
        .summary = "open a session as a user of /etc/passwd",
        .usage = "login [name]",
        .detail = "Asks for the name (unless it is given) and for the password, three times:\n"
                  "a wrong password after the third answer closes the door. The uid and the\n"
                  "gid of the shell become the ones of the account and the home directory\n"
                  "becomes the directory of the shell, so every program started from then on\n"
                  "runs as that user. Only root may open a session, and the way back to root\n"
                  "is a reboot: this system has one credential per task.\n",
    };
    static const struct kshell_help su_help = {
        .summary = "become another user of /etc/passwd",
        .usage = "su [-|-l] [name]",
        .detail = "Without a name the user is root. Root becomes anybody without a password;\n"
                  "a user is asked for the password of the account it becomes. `-` (or `-l`)\n"
                  "makes the home directory of the account the directory of the shell, as a\n"
                  "login would. The way back to root from a user is a reboot.\n",
    };
    static const struct kshell_help id_help = {
        .summary = "the user of this shell",
        .usage = "id",
        .detail = "The uid, the gid and the account of /etc/passwd that the shell runs as.\n",
    };
    int rc = 0;

    rc |= kshell_register_help("login", &login_help, cmd_login);
    rc |= kshell_register_help("su", &su_help, cmd_su);
    rc |= kshell_register_help("id", &id_help, cmd_id);

    return rc;
}
