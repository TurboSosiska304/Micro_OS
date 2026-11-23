/*
 * Micro OS — the error number of a program and its sentence.
 *
 * The calls answer a negative number and the wrappers of `src/syscall.c` keep
 * its magnitude here, which is the shape a portable library expects: it calls
 * `read` and then reads `errno` without a second call. A program is one
 * thread, so the number is a plain global and not a block per thread.
 */
#include "ulibc.h"

#include <errno.h>

static int g_errno_s;

int *__ulibc_errno(void)
{
    return &g_errno_s;
}

/*
 * The sentences of the codes the system has. A code this list does not know
 * is the sentence "unknown error": the table covers the ABI and does not
 * have to grow with a number the kernel does not use.
 */
char *strerror(int code)
{
    switch (code) {
    case 0:
        return "no error";
    case EPERM:
        return "operation not permitted";
    case ENOENT:
        return "no such file or directory";
    case ESRCH:
        return "no such process";
    case EINTR:
        return "interrupted";
    case EIO:
        return "input/output error";
    case ENOEXEC:
        return "exec format error";
    case EBADF:
        return "bad file descriptor";
    case ECHILD:
        return "no child processes";
    case EAGAIN:
        return "try again";
    case ENOMEM:
        return "out of memory";
    case EFAULT:
        return "bad address";
    case EBUSY:
        return "resource busy";
    case ENOTDIR:
        return "not a directory";
    case EISDIR:
        return "is a directory";
    case E2BIG:
        return "argument list too long";
    case EACCES:
        return "permission denied";
    case EEXIST:
        return "file exists";
    case EXDEV:
        return "cross-device link";
    case ENOTTY:
        return "not a terminal";
    case ERANGE:
        return "result too large";
    case EDOM:
        return "argument out of domain";
    case EINVAL:
        return "invalid argument";
    case EMFILE:
        return "too many open files";
    case ENOSPC:
        return "no space left";
    case ESPIPE:
        return "illegal seek";
    case EPIPE:
        return "broken pipe";
    case ENAMETOOLONG:
        return "name too long";
    case ENOSYS:
        return "not implemented";
    case ENOTEMPTY:
        return "directory not empty";
    case ENOTSUP:
        return "not supported";
    default:
        return "unknown error";
    }
}
