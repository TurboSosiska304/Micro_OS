/*
 * Micro OS — the parts of the library of a program that its files share.
 *
 * The door of the system call is in `src/syscall.c`; the resolver of a path
 * and the directory of the program are in `src/path.c`, and the two families
 * of wrappers that are not one line each (the ends of `unistd.h` and the
 * information of `<sys/stat.h>`) build on them. This header is not part of the
 * library a program includes: it is the seam between the files of the library.
 */
#ifndef MICRO_OS_LIBC_USER_INTERNAL_H
#define MICRO_OS_LIBC_USER_INTERNAL_H

#include "ulibc.h"

/* The one door: the number of the call, six arguments, the raw answer. */
long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5);

/* The answer of a call with the POSIX shape: -1 and errno, or the number. */
long __ulibc_result(long result);

/*
 * The path of a call, resolved against the directory of the program: an
 * absolute path is copied, a relative one is joined to the cwd, `.` and `..`
 * are folded, and a path that does not fit answers NULL with errno set. The
 * buffer belongs to the library and is valid until the next four calls.
 */
const char *__ulibc_path(const char *path);

/* The directory of the program; `__ulibc_chdir` answers 0 or -1. */
const char *__ulibc_cwd(void);
int __ulibc_chdir(const char *path);

/* The information of a path in the POSIX structure; 0 or -1. */
int __ulibc_stat_path(const char *path, void *info);

/*
 * The directory a descriptor names. A directory is opened by a path and the
 * ABI has no call relative to an open directory (`openat`, `fstatat`), so the
 * library keeps the text of the last few directories it opened: the helpers
 * of a recursive walk ask for the parent, and this is where the parent is.
 */
void __ulibc_dir_remember(int fd, const char *path);
void __ulibc_dir_forget(int fd);
const char *__ulibc_dir_path(int fd);

#endif /* MICRO_OS_LIBC_USER_INTERNAL_H */
