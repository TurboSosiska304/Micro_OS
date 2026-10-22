/*
 * Micro OS — the system calls of the C library.
 *
 * Every function here is a thin layer over the ABI of the system calls.
 * The library does not know how the kernel works, it only knows the
 * contract of the calls.
 */
#include "libc/unistd.h"

#include "kernel/errno.h"
#include "kernel/ksys.h"

/*
 * The code of the last error belongs to the task that made the call: two
 * tasks that use the library at the same time must not read each other's
 * error. The standard says the same about errno (C11 7.5).
 */
static _Thread_local int g_errno;

/* The code of the last error. The numbers are the numbers of POSIX. */
int microos_errno(void)
{
    return g_errno;
}

void microos_set_errno(int code)
{
    g_errno = (code > 0) ? code : -code;
}

/* Turn the negative result of a call into the -1 of POSIX. */
static int fail(int rc)
{
    if (rc < 0) {
        g_errno = -rc;
    } else {
        g_errno = KERR_IO;
    }
    return -1;
}

/* The flags of POSIX to the flags of the kernel. */
static int map_flags(int flags)
{
    int out = 0;
    int access = flags & 3;

    if (access == MICROOS_O_WRONLY) {
        out |= KSYS_O_WRONLY;
    } else if (access == MICROOS_O_RDWR) {
        out |= KSYS_O_RDWR;
    }

    if (flags & MICROOS_O_CREAT) {
        out |= KSYS_O_CREAT;
    }
    if (flags & MICROOS_O_TRUNC) {
        out |= KSYS_O_TRUNC;
    }
    if (flags & MICROOS_O_APPEND) {
        out |= KSYS_O_APPEND;
    }

    return out;
}

/*
 * The text of an error code. The code is the number of POSIX (the same
 * number that microos_errno() gives), the text is the usual one.
 */
const char *microos_strerror(int code)
{
    switch (code) {
    case 0:
        return "no error";
    case -KERR_PERM:
        return "operation not permitted";
    case -KERR_NOENT:
        return "no such file or directory";
    case -KERR_EXIST:
        return "file exists";
    case -KERR_IO:
        return "input/output error";
    case -KERR_TOOBIG:
        return "argument list too long";
    case -KERR_BADF:
        return "bad file descriptor";
    case -KERR_AGAIN:
        return "try again later";
    case -KERR_NOSYS:
        return "function not implemented";
    case -KERR_NOTDIR:
        return "not a directory";
    case -KERR_ISDIR:
        return "is a directory";
    case -KERR_INVAL:
        return "invalid argument";
    case -KERR_NOSPC:
        return "no space left on device";
    case -KERR_NOTEMPTY:
        return "directory not empty";
    default:
        return "unknown error";
    }
}

int microos_open(const char *path, int flags, ...)
{
    int rc;

    if (path == NULL) {
        return fail(KERR_INVAL);
    }

    /* The access of POSIX is two bits: 3 is not a mode. */
    if ((flags & 3) == 3) {
        return fail(KERR_INVAL);
    }

    /*
     * The mode, when the caller gives one, stays in the register it was
     * passed in: the ABI keeps the third argument of open for it, and the
     * filesystem does not use it yet. It is not read here, because a
     * caller may not have passed it at all.
     */
    rc = ksys_open(path, map_flags(flags));
    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return rc;
}

int microos_close(int fd)
{
    int rc = ksys_close(fd);

    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return 0;
}

long microos_read(int fd, void *buf, size_t count)
{
    long rc = (long)ksys_read(fd, buf, (k_size_t)count);

    if (rc < 0) {
        return fail((int)rc);
    }

    g_errno = 0;

    return rc;
}

long microos_write(int fd, const void *buf, size_t count)
{
    long rc = (long)ksys_write(fd, buf, (k_size_t)count);

    if (rc < 0) {
        return fail((int)rc);
    }

    g_errno = 0;

    return rc;
}

long microos_lseek(int fd, long offset, int whence)
{
    long rc = (long)ksys_lseek(fd, (k_ssize_t)offset, whence);

    if (rc < 0) {
        return fail((int)rc);
    }

    g_errno = 0;

    return rc;
}

int microos_unlink(const char *path)
{
    int rc;

    if (path == NULL) {
        return fail(KERR_INVAL);
    }

    rc = ksys_unlink(path);
    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return 0;
}

int microos_mkdir(const char *path)
{
    int rc;

    if (path == NULL) {
        return fail(KERR_INVAL);
    }

    rc = ksys_mkdir(path);
    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return 0;
}

int microos_waitpid(int pid, int *status, int flags)
{
    int rc = ksys_waitpid(pid, status, (flags & MICROOS_WNOHANG) ? KSYS_WNOHANG : 0);

    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return rc;
}

int microos_getpid(void)
{
    return (int)ksys_getpid();
}

int microos_getppid(void)
{
    return (int)ksys_getppid();
}

int microos_getcwd(char *buf, size_t size)
{
    int rc;

    if (buf == NULL || size == 0) {
        return fail(KERR_INVAL);
    }

    rc = ksys_getcwd(buf, (k_size_t)size);
    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return 0;
}

unsigned microos_sleep(unsigned ms)
{
    if (ksys_sleep((ksys_arg_t)ms) < 0) {
        fail(KERR_AGAIN);
    }

    return 0;
}

int microos_yield(void)
{
    return ksys_yield();
}

void microos_exit(int code)
{
    ksys_call(KSYS_EXIT, (ksys_arg_t)code, 0, 0, 0, 0, 0);

    /* The call does not return, but the compiler does not know it. */
    for (;;) {
        microos_yield();
    }
}
