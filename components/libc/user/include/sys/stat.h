/*
 * Micro OS — the information about an object, for a portable source.
 *
 * The kernel has two calls: `stat` with the type, the size, the mode and the
 * owner, and `statx` with the two moments as well. A program that includes
 * `<sys/stat.h>` gets the POSIX structure those calls fill, and the macros
 * `S_IS*` read it. There are no links and no devices in the VFS of this
 * system, so `lstat` is `stat` and a special file is whatever the filesystem
 * of /dev answers.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_STAT_H
#define MICRO_OS_LIBC_USER_SYS_STAT_H

#include "../ulibc.h"
#include "types.h"

#ifndef MICRO_OS_LIBC_USER_MODE_T_DEFINED
#define MICRO_OS_LIBC_USER_MODE_T_DEFINED
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef unsigned long nlink_t;
typedef long blksize_t;
typedef long blkcnt_t;
#endif

#ifndef MICRO_OS_LIBC_USER_TIME_T_DEFINED
#define MICRO_OS_LIBC_USER_TIME_T_DEFINED
typedef long time_t;
#endif

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

/* The type of an object, the mask and its values. */
#define S_IFMT 0170000
#define S_IFSOCK 0140000
#define S_IFLNK 0120000
#define S_IFREG 0100000
#define S_IFBLK 0060000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFIFO 0010000

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

#define S_IRWXU (S_IRUSR | S_IWUSR | S_IXUSR)
#define S_IRWXG (S_IRGRP | S_IWGRP | S_IXGRP)
#define S_IRWXO (S_IROTH | S_IWOTH | S_IXOTH)

#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)

int stat(const char *path, struct stat *info);
int lstat(const char *path, struct stat *info);
int fstat(int fd, struct stat *info);
int fstatat(int directory, const char *path, struct stat *info, int flags);
int chmod(const char *path, mode_t mode);
mode_t umask(mode_t mask);
int mkdir(const char *path, ...);

#endif /* MICRO_OS_LIBC_USER_SYS_STAT_H */
