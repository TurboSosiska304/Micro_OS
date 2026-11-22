/*
 * Micro OS — the one user and the one group of the system.
 *
 * The kernel has uid 0 and gid 0 and no file of accounts: the records of a
 * filesystem hold a uid of fifteen bits, and every object of this machine was
 * made by the same user. A portable source still asks for a name, so a listing
 * prints `root` where a number would say nothing.
 *
 * The structure is the one of POSIX and the strings are static: a caller does
 * not free the answer and does not keep it across a call of this file.
 */
#include <grp.h>
#include <pwd.h>

#include <errno.h>

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
