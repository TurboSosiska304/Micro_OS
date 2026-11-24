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

/*
 * The words of a line, split in place with the quoting rules of the shell of
 * the machine: a word ends at a blank, `'...'` and `"..."` keep their blanks,
 * and `\` escapes. The answer is the number of words, 0 for a line of blanks,
 * or -1 with `E2BIG` for a line longer than the caller's table. `src/words.c`
 * is the one implementation, and `popen`/`system` of `src/popen.c` and
 * `src/system.c` are the callers.
 */
int __ulibc_split_words(char *text, char **words, int max);

/*
 * The words of one command, in the block of the caller: the copy of the text,
 * the split and the NULL that ends the vector, in one call. The two numbers
 * are the ones of one `execve` of the ABI - eight words in a block of 512
 * bytes (`KSYS_ARGV_MAX` and `KSYS_ARGV_BYTES` of the kernel) - because a
 * longer vector would be refused by the call and not by this library.
 */
#define ULIBC_COMMAND_WORDS 8
#define ULIBC_COMMAND_BYTES 512

int __ulibc_command_words(const char *command, char *block, char **words, int max);

/*
 * A new descriptor (an `open`, an end of a `pipe`) off the numbers of the
 * console: 1 and 2 are the console of this system while they are not open as
 * a file, so a file that took one of them receives what the program writes
 * "to the console" (docs/debts.md). The answer is the number the descriptor
 * lives at after the move - the same number when it was never 1 or 2, and
 * `other` is the end of a pipe that the move must not take.
 */
int __ulibc_fd_away(int fd, int other);

#endif /* MICRO_OS_LIBC_USER_INTERNAL_H */
