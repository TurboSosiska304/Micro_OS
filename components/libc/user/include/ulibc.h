/*
 * Micro OS — the library of a program of the x86-64 port.
 *
 * This is not the library of the kernel (components/libc/src): that one is
 * linked into the kernel and calls the wrappers of `ksys_*`, which a program
 * of ring 3 cannot reach. Here every function is a thin layer over the one
 * door a program has - `int $0x80` (ADR 0006, ADR 0016) - and nothing else.
 *
 * The numbers of the calls and the structures they speak come from the same
 * header the kernel is compiled against (kernel/ksys.h, which carries the VFS
 * structures with it), so a number or a field can never drift between the two
 * sides of the door.
 *
 * The entry of a program of this port is the C entry with three arguments
 * (ADR 0016):
 *
 *     int main(int argc, char *const argv[], char *const envp[]);
 *
 * A program linked against this library starts in `_start` (src/start.c),
 * which calls main() and then exit() with its result; a program does not
 * return from main() into anything, because there is no kernel frame under
 * it. A program that ends with `ret` instead faults on address zero, which is
 * the guard the port leaves at the top of the stack.
 *
 * The result of a call is the result of the system call: a negative number is
 * the error with its minus sign (-2 is ENOENT), exactly as kernel/errno.h
 * names it. The library does not keep an errno of its own; a program that
 * wants the POSIX shape can use the sign.
 */
#ifndef MICRO_OS_LIBC_USER_ULIBC_H
#define MICRO_OS_LIBC_USER_ULIBC_H

#include <stddef.h>

/* The numbers and the structures of the calls, from the kernel's own header. */
#include "kernel/ksys.h"

/* The descriptors a program knows without opening them. The console of the
 * port is 1 and 2. Descriptor 0 is the input of a program: the shell does not
 * pass its own table to a program yet (docs/debts.md), so it is open only
 * when the program opened it - which is the first descriptor a fresh table
 * gives out - and a read of it answers EBADF otherwise. */
#define ULIBC_STDIN 0
#define ULIBC_STDOUT 1
#define ULIBC_STDERR 2

/* The flags of open, the numbers of POSIX; open() maps them to the ABI. */
#define ULIBC_O_RDONLY 0
#define ULIBC_O_WRONLY 1
#define ULIBC_O_RDWR 2
#define ULIBC_O_CREAT 0100
#define ULIBC_O_TRUNC 01000
#define ULIBC_O_APPEND 02000
#define ULIBC_O_DIRECTORY 0200000

/* The whence of lseek, the numbers of POSIX. */
#define ULIBC_SEEK_SET 0
#define ULIBC_SEEK_CUR 1
#define ULIBC_SEEK_END 2

#define ULIBC_EXIT_SUCCESS 0
#define ULIBC_EXIT_FAILURE 1

/* --- the calls --- */

int open(const char *path, int flags, ...);
int close(int fd);
long read(int fd, void *buf, size_t count);
long write(int fd, const void *buf, size_t count);
long lseek(int fd, long offset, int whence);
/*
 * The next entries of a directory opened with ULIBC_O_DIRECTORY. `count` is a
 * number of bytes, not of entries, and has to be a whole number of them; the
 * answer is the number of bytes written or a negative error. The end of the
 * directory is an empty answer, not an error.
 */
long readdir(int fd, struct vfs_dirent *entries, size_t count);

/*
 * The objects of the tree: the information of a path, a directory, a name
 * removed. `stat` answers 0 and fills the structure, `mkdir` and `unlink`
 * answer 0; a negative number is the error of the call.
 */
int stat(const char *path, struct vfs_stat *info);
int mkdir(const char *path);
int unlink(const char *path);

/*
 * The current directory of the task, in the buffer of the caller: the path
 * the shell had when it started the program. The answer is `buf`, or NULL
 * when the call refused; the text is terminated in every case.
 */
char *getcwd(char *buf, size_t size);

int getpid(void);
long uptime(void);

/*
 * Make this task a program: the path is read through the VFS, the mode and
 * the image are checked, and a new task runs the program while this one waits
 * for it (ADR 0016). The environment is a vector of "NAME=VALUE" words; NULL
 * is the environment of the system. The answer is 0 when the program ended,
 * and a negative error when it did not start; the status of the program itself
 * goes to whoever waits for this task.
 */
int execve(const char *path, char *const argv[], char *const envp[]);

/*
 * End the program. The kernel takes the task down and the caller of execve
 * waits for it; the low byte of the code is the status it sees. This call
 * does not return.
 */
void exit(int code) __attribute__((noreturn));

/* --- the strings --- */

size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
/*
 * The first place of `needle` in `text`, or NULL. An empty needle is the
 * text itself, as in the library of a Unix-like system.
 */
char *strstr(const char *text, const char *needle);
void *memcpy(void *dest, const void *source, size_t count);
void *memset(void *dest, int byte, size_t count);

/* --- the console --- */

/*
 * The conversions of the formatter: %d, %i, %u, %x, %X, %c, %s, %p and %%.
 * A width and the flags '-' and '0' are honoured; the length is 'l' or 'z'
 * for a long, and the answer is the number of bytes written or a negative
 * error. The text goes to the console through write(1, ...), in pieces of a
 * fixed buffer, so no allocation is involved.
 */
int printf(const char *format, ...);
int puts(const char *text);

#endif /* MICRO_OS_LIBC_USER_ULIBC_H */
