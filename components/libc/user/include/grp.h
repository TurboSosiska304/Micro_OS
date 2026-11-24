/*
 * Micro OS — the group of a name and of a gid.
 *
 * A group is a line of /etc/group, `name:gid:member,member` (ADR 0044 §4),
 * and `getgrgid` and `getgrnam` read the file the way `getpwuid` reads
 * /etc/passwd: there is no table of groups in the library and none in the
 * kernel. A task has one gid and no supplementary list, so the member list is
 * parsed and returned and decides no permission.
 *
 * The structure is the one of POSIX and the strings are static: a caller does
 * not free the answer and does not keep it across a call of this file.
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
