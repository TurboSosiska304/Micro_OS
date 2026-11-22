/*
 * Micro OS — the one group of the system.
 *
 * The same story as `pwd.h`: uid 0 and gid 0 are the only ones the kernel
 * knows, and a listing names the group `root`.
 */
#ifndef MICRO_OS_LIBC_USER_GRP_H
#define MICRO_OS_LIBC_USER_GRP_H

#include <sys/types.h>

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);

#endif /* MICRO_OS_LIBC_USER_GRP_H */
