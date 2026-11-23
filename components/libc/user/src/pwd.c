/*
 * Micro OS — the user and the group of the calling program.
 *
 * A name is a line of /etc/passwd: `name:uid:gid:home:shell`, the five fields
 * of this system. `getpwnam` and `getpwuid` read the file through the door of
 * the system calls and answer a record of it — the same file the shell reads
 * for `login`, so a program and the console cannot disagree about who uid 7
 * is. There is no table of names in the library and none in the kernel.
 *
 * The kernel keeps a uid and a gid in every task (a fork and a program keep
 * them), so `getuid` and its siblings ask the kernel and answer the identity
 * the program runs with; a program that an administrator gave another name
 * answers with that name. There is no group file yet: `getgrgid` and
 * `getgrnam` know the group of root and nothing else (docs/debts.md).
 *
 * The structure is the one of POSIX and the strings are static: a caller does
 * not free the answer and does not keep it across a call of this file.
 */
#include <grp.h>
#include <pwd.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "ulibc_internal.h"

/* The files of the accounts, and the room a record of one takes. A file of a
 * machine of a few people is a few hundred bytes; one that is larger is not
 * read to its end, which is the same rule the shell follows. */
#define ULIBC_PASSWD_PATH "/etc/passwd"
#define ULIBC_PASSWD_MAX 512
#define ULIBC_PASSWD_LINE 160
#define ULIBC_PASSWD_FIELDS 5

static char g_user_name_s[16];
static char g_user_password_s[2];
static char g_user_dir_s[48];
static char g_user_shell_s[48];

static struct passwd g_user_s = {
    .pw_name = g_user_name_s,
    .pw_passwd = g_user_password_s,
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_gecos = g_user_name_s,
    .pw_dir = g_user_dir_s,
    .pw_shell = g_user_shell_s,
};

static char g_group_name_s[] = "root";
static char g_group_password_s[] = "";
static char *const g_group_members_s[] = {NULL};

static struct group g_group_s = {
    .gr_name = g_group_name_s,
    .gr_passwd = g_group_password_s,
    .gr_gid = 0,
    .gr_mem = (char **)g_group_members_s,
};

/*
 * The whole file, NUL terminated. The answer is the length, or -1 when the
 * file cannot be read: a program that cannot open /etc/passwd has no name and
 * says so with errno, which is the answer of a caller of getpwnam.
 */
static int read_passwd(char *buf, size_t size)
{
    long fd;
    size_t used = 0;

    if (size < 2) {
        errno = EINVAL;
        return -1;
    }

    fd = __ulibc_result(
        __ulibc_syscall6(KSYS_OPEN, (long)(uintptr_t)ULIBC_PASSWD_PATH, KSYS_O_RDONLY, 0, 0, 0, 0));
    if (fd < 0) {
        return -1;
    }

    for (;;) {
        long n;

        if (used + 1 >= size) {
            break;
        }

        n = __ulibc_result(__ulibc_syscall6(KSYS_READ, fd, (long)(uintptr_t)(buf + used),
                                            (long)(size - 1 - used), 0, 0, 0));
        if (n < 0) {
            (void)__ulibc_syscall6(KSYS_CLOSE, fd, 0, 0, 0, 0, 0);
            return -1;
        }
        if (n == 0) {
            break;
        }

        used += (size_t)n;
    }

    (void)__ulibc_syscall6(KSYS_CLOSE, fd, 0, 0, 0, 0, 0);
    buf[used] = '\0';

    return (int)used;
}

/* A field of a line copied into a string: false when it does not fit. */
static bool copy_field(const char **at, char *out, size_t size)
{
    const char *start = *at;
    size_t len = 0;

    while (start[len] != '\0' && start[len] != ':' && start[len] != '\n') {
        len++;
    }

    if (len + 1 > size) {
        return false;
    }

    memcpy(out, start, len);
    out[len] = '\0';

    *at = start + len;
    if (**at == ':') {
        (*at)++;
    }

    return true;
}

/* A field as a number: false on anything that is not a decimal number. */
static bool number_field(const char **at, unsigned *out)
{
    const char *start = *at;
    unsigned value = 0;
    size_t digits = 0;

    while (start[digits] >= '0' && start[digits] <= '9') {
        value = value * 10u + (unsigned)(start[digits] - '0');
        digits++;
    }

    if (digits == 0 || (start[digits] != '\0' && start[digits] != ':' && start[digits] != '\n')) {
        return false;
    }

    *at = start + digits;
    if (**at == ':') {
        (*at)++;
    }

    *out = value;

    return true;
}

/*
 * One line of the file into the static record. False when the line is not a
 * record of this format (a comment, an empty line, a field that does not fit),
 * which the caller skips: a file of accounts may hold a line of another
 * system beside its own, and a program that reads it is not the shell that
 * refuses a session for it.
 */
static bool parse_line(const char *line, struct passwd *out)
{
    const char *at = line;
    unsigned uid;
    unsigned gid;

    if (line[0] == '\0' || line[0] == '\n' || line[0] == '#') {
        return false;
    }

    if (!copy_field(&at, g_user_name_s, sizeof(g_user_name_s)) || g_user_name_s[0] == '\0') {
        return false;
    }
    if (!number_field(&at, &uid) || !number_field(&at, &gid)) {
        return false;
    }
    if (!copy_field(&at, g_user_dir_s, sizeof(g_user_dir_s))) {
        return false;
    }
    if (!copy_field(&at, g_user_shell_s, sizeof(g_user_shell_s))) {
        return false;
    }

    out->pw_uid = (uid_t)uid;
    out->pw_gid = (gid_t)gid;

    return true;
}

/* The record of the first line that matches, by name or by uid. */
static struct passwd *find(bool by_uid, const char *name, uid_t uid)
{
    static char text[ULIBC_PASSWD_MAX];
    const char *at;
    int length;

    length = read_passwd(text, sizeof(text));
    if (length < 0) {
        return NULL;
    }

    at = text;

    while (*at != '\0') {
        const char *end = strchr(at, '\n');
        size_t len = (end != NULL) ? (size_t)(end - at) : strlen(at);
        char line[ULIBC_PASSWD_LINE];

        if (len < sizeof(line)) {
            memcpy(line, at, len);
            line[len] = '\0';

            if (parse_line(line, &g_user_s)) {
                if (by_uid ? (g_user_s.pw_uid == uid) : (strcmp(g_user_s.pw_name, name) == 0)) {
                    return &g_user_s;
                }
            }
        }

        at = (end != NULL) ? end + 1 : at + len;
    }

    errno = ENOENT;

    return NULL;
}

struct passwd *getpwuid(uid_t uid)
{
    return find(true, NULL, uid);
}

struct passwd *getpwnam(const char *name)
{
    if (name == NULL) {
        errno = ENOENT;
        return NULL;
    }

    return find(false, name, 0);
}

/*
 * The credentials of the program, from the task that runs it. The kernel
 * keeps one uid and not the three of POSIX, so geteuid answers what getuid
 * answers; the two calls stay apart because a source that asks for one must
 * not have to know that (ADR 0005).
 */
uid_t getuid(void)
{
    return (uid_t)__ulibc_result(__ulibc_syscall6(KSYS_GETUID, 0, 0, 0, 0, 0, 0));
}

uid_t geteuid(void)
{
    return (uid_t)__ulibc_result(__ulibc_syscall6(KSYS_GETEUID, 0, 0, 0, 0, 0, 0));
}

gid_t getgid(void)
{
    return (gid_t)__ulibc_result(__ulibc_syscall6(KSYS_GETGID, 0, 0, 0, 0, 0, 0));
}

int setuid(uid_t uid)
{
    return (int)__ulibc_result(__ulibc_syscall6(KSYS_SETUID, (long)uid, 0, 0, 0, 0, 0));
}

int setgid(gid_t gid)
{
    return (int)__ulibc_result(__ulibc_syscall6(KSYS_SETGID, (long)gid, 0, 0, 0, 0, 0));
}

struct group *getgrgid(gid_t gid)
{
    if (gid != 0) {
        errno = ENOENT;
        return NULL;
    }

    return &g_group_s;
}

struct group *getgrnam(const char *name)
{
    if (name == NULL || strcmp(name, g_group_name_s) != 0) {
        errno = ENOENT;
        return NULL;
    }

    return &g_group_s;
}
