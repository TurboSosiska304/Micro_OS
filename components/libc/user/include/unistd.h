/*
 * Micro OS — the calls of a program under the names POSIX gives them.
 *
 * The prototypes are the ones of `ulibc.h`; the file adds the names a portable
 * source expects, the numbers of `lseek`, and the calls the library builds
 * over the ABI: `access` over a stat, `rmdir` over `KSYS_RMDIR` (the removal
 * of a directory, which is not the unlink of a file), `execvp` over `execve`
 * and the words of `PATH`. `chdir` is the call of the ABI (number 48), so a
 * program moves the directory of its task and the program it starts begins
 * in it; a call of POSIX that this system cannot answer - `readlink`, because
 * a symbolic link is not an object of this filesystem - answers -1 and
 * `ENOSYS`; the sentence is in `docs/porting-programs.md`.
 *
 * The types of a file are declared here as well as in `<sys/types.h>`, under
 * one guard, because a source may include either header first and the two are
 * one set.
 */
#ifndef MICRO_OS_LIBC_USER_UNISTD_H
#define MICRO_OS_LIBC_USER_UNISTD_H

#include "ulibc.h"

/* The types of a POSIX header: `uid_t` and `gid_t` of the one user, and the
 * `off_t`/`ssize_t` the rest of this header uses. */
#include <sys/types.h>

#ifndef MICRO_OS_LIBC_USER_TYPES_DEFINED
#define MICRO_OS_LIBC_USER_TYPES_DEFINED
typedef long ssize_t;
typedef long off_t;
typedef long long off64_t;
#endif

#ifndef MICRO_OS_LIBC_USER_PID_T_DEFINED
#define MICRO_OS_LIBC_USER_PID_T_DEFINED
typedef int pid_t;
#endif

#define STDIN_FILENO ULIBC_STDIN
#define STDOUT_FILENO ULIBC_STDOUT
#define STDERR_FILENO ULIBC_STDERR

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* The names of `sysconf` this machine can answer. */
#define _SC_ARG_MAX 0
#define _SC_LOGIN_NAME_MAX 32
#define _POSIX_ARG_MAX 4096

long sysconf(int name);

/* The calls of the library of a program that a source expects from here. */
int close(int fd);
ssize_t read(int fd, void *buffer, size_t count);
ssize_t write(int fd, const void *buffer, size_t count);
off_t lseek(int fd, off_t offset, int whence);
/*
 * The read at an offset of a file, for a library that reads the tail of it
 * without moving the descriptor (libmagic reads the end of a core file). The
 * port answers it with `src/pread.c` of `third_party/file`, because the VFS
 * has no call of its own yet.
 */
ssize_t pread(int fd, void *buffer, size_t count, off_t offset);
int unlink(const char *path);

/*
 * The two calls of a tree this system does not have: a symbolic link is not
 * an object of MicroFS and the canonical name of a path needs a walk of the
 * filesystem. They answer `ENOSYS` (src/unsupported.c), so a portable source
 * links and its own fallback runs (`docs/debts.md`).
 */
char *realpath(const char *path, char *buffer);
int symlink(const char *target, const char *path);

int rmdir(const char *path);
int access(const char *path, int mode);
int chdir(const char *path);
int isatty(int fd);
ssize_t readlink(const char *path, char *buffer, size_t size);
int pipe(int fds[2]);

/* The heap of the program: brk moves the end and answers 0, sbrk moves it by
 * a number of bytes and answers the address the room began at. */
int brk(void *addr);
void *sbrk(long increment);
int dup2(int oldfd, int newfd);
pid_t fork(void) __attribute__((returns_twice));
pid_t getpid(void);
pid_t getppid(void);
/*
 * The credentials of the program: the uid and the gid of the task, and the
 * two calls that change them. Root may take any number; a program of another
 * uid may only name the one it has. There is one uid, so geteuid answers what
 * getuid answers, and the group decides no permission of a file yet.
 */
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int gethostname(char *name, size_t size);
pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
int execvp(const char *file, char *const argv[]);
void _exit(int code) __attribute__((__noreturn__));
unsigned sleep(unsigned seconds);
int usleep(unsigned microseconds);

extern char **environ;
extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;
int getopt(int argc, char *const argv[], const char *options);

#endif /* MICRO_OS_LIBC_USER_UNISTD_H */
