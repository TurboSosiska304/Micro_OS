/*
 * Micro OS — one user, one group.
 *
 * A name is a line of /etc/passwd (`name:uid:gid:home:shell`), and the
 * records of the filesystem hold a uid of fifteen bits. A portable source
 * asks `getpwuid` for a name, so a program that prints `ls -l` prints `guest`
 * instead of a number, and `login` reads the same file. There is no group
 * file: `getgrgid` knows the group of root and nothing else (docs/debts.md).
 */
#ifndef MICRO_OS_LIBC_USER_PWD_H
#define MICRO_OS_LIBC_USER_PWD_H

#include <sys/types.h>

struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);

#endif /* MICRO_OS_LIBC_USER_PWD_H */
