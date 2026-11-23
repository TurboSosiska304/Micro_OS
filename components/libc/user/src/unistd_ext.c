/*
 * Micro OS — the calls of POSIX the library builds over the ABI.
 *
 * Every function here has the shape of POSIX: -1 and `errno` on a failure, the
 * number of the call otherwise. The door of the kernel answers the negative
 * error itself, so the wrapper turns it into the -1 a portable source tests
 * against. The calls the ABI does not have are answered with `ENOSYS` and a
 * sentence in `docs/porting-programs.md`, never with a silent success.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ulibc_internal.h"

/* The POSIX shape of a call: the raw answer, or -1 with errno. */
static int posix(int result)
{
    if (result < 0) {
        errno = -result;
        return -1;
    }

    return result;
}

/*
 * A path relative to a directory descriptor. The ABI has no call of this
 * shape: the library keeps the text of the directories it opened, and a walk
 * that opens a child of a directory asks for the parent's text here. A
 * descriptor the library does not know falls back to the path as the caller
 * wrote it, which is right for the directory of the task.
 */
static const char *relative_path(int directory, const char *path, char *buffer)
{
    const char *parent;
    size_t length;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (path[0] == '/' || directory == AT_FDCWD) {
        return path;
    }

    parent = __ulibc_dir_path(directory);
    if (parent == NULL) {
        return path;
    }

    length = strlen(parent);
    if (length + 1 + strlen(path) + 1 > PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    memcpy(buffer, parent, length);
    if (length > 0 && parent[length - 1] != '/') {
        buffer[length++] = '/';
    }
    strcpy(buffer + length, path);

    return buffer;
}

int openat(int directory, const char *path, int flags, ...)
{
    char buffer[PATH_MAX];
    const char *full = relative_path(directory, path, buffer);

    if (full == NULL) {
        return -1;
    }

    return open(full, flags);
}

int fstatat(int directory, const char *path, struct stat *info, int flags)
{
    char buffer[PATH_MAX];
    const char *full = relative_path(directory, path, buffer);

    (void)flags; /* no links in the VFS: the two calls are one */

    if (full == NULL) {
        return -1;
    }

    return stat(full, info);
}

long sysconf(int name)
{
    if (name == _SC_ARG_MAX) {
        return _POSIX_ARG_MAX + 512; /* the block of the ABI is 512 bytes */
    }
    if (name == _SC_LOGIN_NAME_MAX) {
        return 32; /* the longest name of a user of this machine */
    }

    errno = EINVAL;
    return -1;
}

int access(const char *path, int mode)
{
    struct stat info;

    if (stat(path, &info) != 0) {
        return -1;
    }

    /* The system has one user, uid 0, and its rule is that root gets nothing
     * from the mode: an execute bit still has to be there for a program. */
    if (mode == F_OK) {
        return 0;
    }
    if ((mode & X_OK) != 0 && (info.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
        errno = EACCES;
        return -1;
    }

    return 0;
}

int chdir(const char *path)
{
    return __ulibc_chdir(path);
}

int rmdir(const char *path)
{
    const char *resolved = __ulibc_path(path);

    if (resolved == NULL) {
        return -1;
    }

    return posix((int)__ulibc_syscall6(KSYS_RMDIR, (long)(unsigned long)resolved, 0, 0, 0, 0, 0));
}

ssize_t readlink(const char *path, char *buffer, size_t size)
{
    (void)path;
    (void)buffer;
    (void)size;

    /* The VFS has no symbolic links; a caller that asks for the target of one
     * gets the error of a name that is not a link. */
    errno = EINVAL;
    return -1;
}

int pipe(int fds[2])
{
    return posix((int)__ulibc_syscall6(KSYS_PIPE, (long)(unsigned long)fds, 0, 0, 0, 0, 0));
}

int dup2(int oldfd, int newfd)
{
    return posix((int)__ulibc_syscall6(KSYS_DUP2, oldfd, newfd, 0, 0, 0, 0));
}

pid_t fork(void)
{
    /*
     * The POSIX shape is the call itself: the port copies the context of the
     * caller, the child returns from this same instruction with zero in RAX
     * and the parent with the pid. The pages of the program are copy-on-write
     * after the fork, so the page under the stack of this call is the child's
     * as soon as either side writes it - the parent does not need a yield to
     * keep out of the return of the child.
     */
    return (pid_t)posix((int)__ulibc_syscall6(KSYS_FORK, 0, 0, 0, 0, 0, 0));
}

pid_t getppid(void)
{
    return (pid_t)__ulibc_syscall6(KSYS_GETPPID, 0, 0, 0, 0, 0, 0);
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    return (pid_t)posix(
        (int)__ulibc_syscall6(KSYS_WAITPID, pid, (long)(unsigned long)status, options, 0, 0, 0));
}

pid_t wait(int *status)
{
    return waitpid(-1, status, 0);
}

void _exit(int code)
{
    (void)__ulibc_syscall6(KSYS_EXIT, code, 0, 0, 0, 0, 0);

    for (;;) {
    }
}

unsigned sleep(unsigned seconds)
{
    (void)__ulibc_syscall6(KSYS_SLEEP, (long)seconds * 1000L, 0, 0, 0, 0, 0);

    return 0;
}

/*
 * The words of PATH: `execvp` tries each directory for a name without a
 * slash. The environment of a program is the four words the shell gives it
 * (`/etc/setting` is not an environment), and the library has no `getenv` of
 * its own, so PATH is the one the system promises: `/bin`.
 */
int execvp(const char *file, char *const argv[])
{
    static const char path[] = "/bin";
    char candidate[64];
    char *const envp[] = {"PATH=/bin", "HOME=/", "USER=root", "PWD=/", NULL};

    if (file == NULL || file[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    if (strchr(file, '/') != NULL) {
        /*
         * `execve` already answers -1 with `errno` of the call; wrapping it in
         * `posix()` would translate the -1 a second time and lose the reason
         * a caller prints.
         */
        return execve(file, argv, envp);
    }

    if (strlen(file) + sizeof(path) + 1 > sizeof(candidate)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    strcpy(candidate, path);
    strcat(candidate, "/");
    strcat(candidate, file);

    return execve(candidate, argv, envp);
}
