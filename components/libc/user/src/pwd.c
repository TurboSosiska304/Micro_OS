/*
 * Micro OS — the user and the group of the calling program.
 *
 * The kernel keeps a uid and a gid in every task (a fork and a program keep
 * them) and the records of a filesystem hold a uid of fifteen bits. There is
 * still no file of accounts: a name is known only for root, and any other
 * number is printed as the number it is. `getuid` and its siblings ask the
 * kernel, so a program that an administrator gave another identity answers
 * with that identity.
 *
 * The structure is the one of POSIX and the strings are static: a caller does
 * not free the answer and does not keep it across a call of this file.
 */
#include <grp.h>
#include <pwd.h>

#include <errno.h>
#include <unistd.h>

#include "ulibc_internal.h"

static char g_user_name_s[] = "root";
static char g_user_password_s[] = "";
static char g_user_gecos_s[] = "root";
static char g_user_dir_s[] = "/";
static char g_user_shell_s[] = "/bin/sh";

static struct passwd g_user_s = {
    .pw_name = g_user_name_s,
    .pw_passwd = g_user_password_s,
    .pw_uid = 0,
    .pw_gid = 0,
    .pw_gecos = g_user_gecos_s,
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

struct passwd *getpwuid(uid_t uid)
{
    if (uid != 0) {
        errno = ENOENT;
        return NULL;
    }

    return &g_user_s;
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

struct passwd *getpwnam(const char *name)
{
    if (name == NULL || strcmp(name, g_user_name_s) != 0) {
        errno = ENOENT;
        return NULL;
    }

    return &g_user_s;
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
