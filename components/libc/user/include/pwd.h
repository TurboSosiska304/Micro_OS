/*
 * Micro OS — one user, one group.
 *
 * The system has a single user, uid 0, and a single group, gid 0; the kernel
 * grants it nothing extra and the records of the filesystem hold a uid of
 * fifteen bits. A portable source still asks `getpwuid` for a name, so a
 * program that prints `ls -l` prints `root` instead of a number.
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
