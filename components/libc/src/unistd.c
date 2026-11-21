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
#include "kernel/vfs.h"

/*
 * The structure the program passes and the one the kernel copies are the
 * same bytes: a field moved in one of the two headers would send the moments
 * of the call to the wrong place.
 */
_Static_assert(sizeof(struct microos_statx) == sizeof(struct vfs_statx),
               "the structure of statx and the ABI of the call changed");

/*
 * The code of the last error belongs to the task that made the call: two
 * tasks that use the library at the same time must not read each other's
 * error. The standard says the same about errno (C11 7.5).
 */
static _Thread_local int g_errno;

/* The widths a mode and a uid have in a record of a filesystem. */
#define LIB_MODE_MASK 0777u
#define LIB_UID_MAX 0x7FFFu

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
    if (flags & MICROOS_O_DIRECTORY) {
        out |= KSYS_O_DIRECTORY;
    }

    return out;
}

/*
 * The text of an error code. The code is the number of POSIX (the same
 * number that microos_errno() gives), the text is the usual one. A caller
 * that passes the result of a system call gives the same number with the
 * minus sign of the call, so the sign is dropped here: both are the error.
 */
const char *microos_strerror(int code)
{
    long number = (code < 0) ? -(long)code : (long)code;

    switch (number) {
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
    case -KERR_CHILD:
        return "no child processes";
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
    case -KERR_NAMETOOLONG:
        return "file name too long";
    case -KERR_NOTEMPTY:
        return "directory not empty";
    case -KERR_NOTSUP:
        return "operation not supported";
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

/*
 * The fork. It is the number of the new task in the parent, and 0 in the
 * child - but the child of this fork is a task that re-runs the program of
 * the parent from its start, so it is not a copy of the parent at the call
 * of the fork: the memory is shared (like CLONE_VM) and the work done
 * before the fork runs again in the child. A real fork waits for the port
 * that has an MMU (KERNEL_NOMMU = 0); this one is here so that the shape
 * of a program is the usual one, today on the board and later on a real
 * processor.
 */
int microos_fork(void)
{
    int rc = ksys_fork();

    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return rc;
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

/*
 * The mode and the owner of a file. A number the record of the filesystem
 * cannot hold is a mistake of the caller and is answered here, before the
 * kernel is asked to cut it to fit: nine bits of a mode, fifteen of a uid.
 */
int microos_chmod(const char *path, unsigned mode)
{
    int rc;

    if (path == NULL || (mode & ~LIB_MODE_MASK) != 0u) {
        return fail(KERR_INVAL);
    }

    rc = ksys_chmod(path, (k_u32_t)mode);
    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return 0;
}

int microos_chown(const char *path, unsigned owner)
{
    int rc;

    if (path == NULL || (owner & ~LIB_UID_MAX) != 0u) {
        return fail(KERR_INVAL);
    }

    rc = ksys_chown(path, (k_u32_t)owner);
    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return 0;
}

int microos_statx(const char *path, struct microos_statx *out)
{
    struct vfs_statx info;
    int rc;

    if (path == NULL || out == NULL) {
        return fail(KERR_INVAL);
    }

    rc = ksys_statx(path, &info);
    if (rc < 0) {
        return fail(rc);
    }

    out->inode = info.inode;
    out->type = info.type;
    out->size = info.size;
    out->mode = info.mode;
    out->owner = info.owner;
    out->mtime = info.mtime;
    out->ctime = info.ctime;

    g_errno = 0;

    return 0;
}

/*
 * The program becomes the caller: a call that enters it does not return. A
 * machine without a process to exit into comes back with 0 when the program
 * has finished, and that zero is the answer here too - the caller is the
 * program again, and there is nothing to report.
 */
int microos_execve(const char *path, char *const argv[])
{
    int rc;

    if (path == NULL) {
        return fail(KERR_INVAL);
    }

    rc = ksys_execve(path, argv);
    if (rc < 0) {
        return fail(rc);
    }

    g_errno = 0;

    return 0;
}
