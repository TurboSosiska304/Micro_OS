/*
 * Micro OS — the entries of a directory, for a portable source.
 *
 * The kernel call `readdir` fills a batch of `struct vfs_dirent`; a program
 * that includes `<dirent.h>` sees the POSIX `DIR` and `struct dirent` over it.
 * The library keeps one batch per open directory and hands the entries out one
 * at a time, so a source walks a directory with the loop it has everywhere. A
 * name of this system is short (VFS_NAME_MAX) and the entry of POSIX is filled
 * with it; a position (`telldir`) is not a call of the ABI and is not here.
 */
#ifndef MICRO_OS_LIBC_USER_DIRENT_H
#define MICRO_OS_LIBC_USER_DIRENT_H

#include "ulibc.h"

typedef struct ulibc_dir DIR;

struct dirent {
    unsigned long d_ino;
    unsigned char d_type;
    char d_name[VFS_NAME_MAX + 1];
};

/* The types of `d_type`, the numbers of POSIX where this system has one. */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
void rewinddir(DIR *dir);
int dirfd(DIR *dir);

#endif /* MICRO_OS_LIBC_USER_DIRENT_H */
