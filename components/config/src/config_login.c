/*
 * Micro OS — `login`, `su` and `id`: the door of a session.
 *
 * The console of this machine has no getty and no program of a console of
 * its own: init makes a session task (config/session.h), the session reads
 * the lines of its terminal and runs them through the shell of the kernel.
 * `login` therefore does not start a shell — one is running — it asks for
 * the name and the password of the terminal of the task and hands the
 * session to that user: the uid and the gid of the task move, the home
 * becomes its directory, and every program the person starts from then on
 * inherits the identity. The prompt does not change, and the line after the
 * login says who the shell is; `id` says it again at any moment.
 *
 * The terminal is the one of the task and not the console of the machine: a
 * session of telnet reads its password from the socket it came in on, which
 * is the defect of 1.6 that the cleanroom wrote down. `session_read_line()`
 * echoes to the same terminal unless the line is a secret, so a password is
 * not repeated on a screen and not in the transcript of a client.
 *
 * `login` is the door of a console that has no session, so only root runs it:
 * a shell that became a user is already somebody's session, and the way to
 * another user — root included — is `su`, which asks for the password of the
 * account it becomes. A task has one credential and no saved uid (ADR 0005,
 * docs/syscall-abi.md), so the shell cannot climb back with `setuid`; it
 * climbs with `su` and the password of root, which is a privileged act of the
 * session and not a program of the disk. That is the price of one credential
 * and the reason the door is the console and not a setuid bit in an image
 * (ADR 0033).
 */
#include "config/users.h"

#include <string.h>

#include "config/session.h"
#include "kernel/log.h"
#include "kernel/task.h"
#include "kshell/kshell.h"

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

static int cmd_login(int argc, char **argv)
{
    struct task *self = task_self();
    struct user_account account;
    char name[USERS_NAME_MAX];
    const char *given = NULL;
    int rc;

    if (argc > 2) {
        kshell_printf("usage: login [name]\n");
        return KSHELL_USAGE;
    }

    if (self == NULL || !task_is_root(self)) {
        kshell_printf("login: this shell is already a session; `su` changes the user\n");
        return KSHELL_ERROR;
    }

    if (argc == 2) {
        given = argv[1];
    } else {
        /*
         * A name typed at the prompt of the command is one answer: an empty
         * one is a person who wanted the shell back, and not a getty that
         * asks again until a name comes. A session of telnet has the other
         * rule (session_authenticate with no name asks again), because the
         * terminal there belongs to that login and not to a shell that is
         * already running.
         */
        bool too_long = false;

        if (!session_read_line("login: ", name, sizeof(name), false, &too_long)) {
            return KSHELL_ERROR;
        }

        if (too_long) {
            kshell_printf("login: the name does not fit\n");
            return KSHELL_ERROR;
        }

        if (name[0] == '\0') {
            kshell_printf("login: a name is needed\n");
            return KSHELL_ERROR;
        }

        given = name;
    }

    rc = session_authenticate(given, &account, SESSION_FAIL_DELAY_MS);
    if (rc != USERS_OK) {
        /* A wrong answer and a name that does not fit are sentences the
         * dialogue has already said; a broken file is not, and the person
         * who reads the message of the file needs it once. */
        if (rc != USERS_EAUTH && rc != USERS_ENOENT) {
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
    char password[SESSION_LINE_MAX];
    const char *name = "root";
    bool home = false;
    bool too_long = false;
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
        bool ok;

        if (!session_read_line("password: ", password, sizeof(password), true, &too_long)) {
            return KSHELL_ERROR;
        }

        ok = !too_long && users_authenticate(account.name, password, &account) == USERS_OK;
        memset(password, 0, sizeof(password));

        if (!ok) {
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
    struct user_group group;
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

    /*
     * The names of the two numbers are the two files of /etc and nothing
     * else: a gid without a line in /etc/group is the number alone, the way
     * `id` of POSIX answers for a group the machine does not name.
     */
    if (users_find_gid(gid, &group) != USERS_OK) {
        group.name[0] = '\0';
    }

    if (users_find_uid(uid, &account) == USERS_OK) {
        if (group.name[0] != '\0') {
            kshell_printf("uid=%u(%s) gid=%u(%s) home=%s shell=%s\n", (unsigned)uid, account.name,
                          (unsigned)gid, group.name, (account.home[0] != '\0') ? account.home : "/",
                          (account.shell[0] != '\0') ? account.shell : "none");
        } else {
            kshell_printf("uid=%u(%s) gid=%u home=%s shell=%s\n", (unsigned)uid, account.name,
                          (unsigned)gid, (account.home[0] != '\0') ? account.home : "/",
                          (account.shell[0] != '\0') ? account.shell : "none");
        }
    } else if (group.name[0] != '\0') {
        kshell_printf("uid=%u gid=%u(%s) (no account of this uid)\n", (unsigned)uid, (unsigned)gid,
                      group.name);
    } else {
        kshell_printf("uid=%u gid=%u (no account of this uid)\n", (unsigned)uid, (unsigned)gid);
    }

    return KSHELL_OK;
}

/*
 * The groups of this shell. A task has one gid and no supplementary list
 * (ADR 0044 §4), so the answer is one name: the name of /etc/group that has
 * the gid, or the number alone when the file does not name it.
 */
static int cmd_groups(int argc, char **argv)
{
    struct task *self = task_self();
    struct user_group group;

    (void)argv;

    if (argc != 1) {
        kshell_printf("usage: groups\n");
        return KSHELL_USAGE;
    }

    if (self == NULL) {
        return KSHELL_ERROR;
    }

    if (users_find_gid(task_get_gid(self), &group) == USERS_OK) {
        kshell_printf("%s\n", group.name);
    } else {
        kshell_printf("%u\n", (unsigned)task_get_gid(self));
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
                  "runs as that user. Only root may open a session; the way back to root is\n"
                  "`logout`, which ends the session and gives the console a new one.\n",
    };
    static const struct kshell_help su_help = {
        .summary = "become another user of /etc/passwd",
        .usage = "su [-|-l] [name]",
        .detail = "Without a name the user is root. Root becomes anybody without a password;\n"
                  "a user is asked for the password of the account it becomes. `-` (or `-l`)\n"
                  "makes the home directory of the account the directory of the shell, as a\n"
                  "login would. The way back to root from a user is `logout` on the console\n"
                  "or the password of root under `su`.\n",
    };
    static const struct kshell_help id_help = {
        .summary = "the user and the group of this shell",
        .usage = "id",
        .detail = "The uid and the gid of the shell, the account of /etc/passwd and the group\n"
                  "of /etc/group that answer for them, the home and the shell of the account.\n",
    };
    static const struct kshell_help groups_help = {
        .summary = "the groups of this shell",
        .usage = "groups",
        .detail = "One name: the line of /etc/group whose gid is the gid of the shell. A task\n"
                  "has one gid and no supplementary group in this system.\n",
    };
    int rc = 0;

    rc |= kshell_register_help("login", &login_help, cmd_login);
    rc |= kshell_register_help("su", &su_help, cmd_su);
    rc |= kshell_register_help("id", &id_help, cmd_id);
    rc |= kshell_register_help("groups", &groups_help, cmd_groups);

    return rc;
}
