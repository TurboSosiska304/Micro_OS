/*
 * Micro OS — the door of a program.
 *
 * On x86-64 the kernel has no function a program can call: the program was
 * not compiled into it, and the address of ksys_call() means nothing in ring
 * 3. The door is the instruction `syscall`, which the port opens with the
 * address of its entry in IA32_LSTAR (ports/port_x86_64/src/syscall.c); a call
 * is the number in RAX with the six arguments in RDI, RSI, RDX, R10, R8 and R9
 * (ADR 0006). RCX and R11 are not arguments because the instruction keeps them
 * for the return address and the flags, and the port reads them back into the
 * frame of the call.
 *
 * The port keeps its vector 128 as well: a program of another compiler, the
 * restorer of a signal handler, and a return of `sigreturn` still take
 * `int $0x80`, and the frame of either instruction is the same 160 bytes. The
 * library of this tree takes the fast one.
 *
 * On the board (ADR 0018) there is no instruction of a system call a program
 * may execute: the vector of ESP-IDF takes `syscall` before the port sees it.
 * The door is the address of a function of the kernel with the shape of
 * ksys_call(), handed to _start; the numbers and the arguments are the same.
 *
 * The register variables pin the arguments to the registers of x86-64: a
 * plain "r" constraint would let the compiler choose, and the kernel reads
 * the registers of the convention and nothing else. The flags, the memory and
 * the two registers the processor keeps are clobbered because the handler and
 * the kernel behind it are free to change them.
 */
#include "ulibc.h"

#include "ulibc_internal.h"

#include <errno.h>
#include <signal.h>

/*
 * The board has a window of 8 KiB of text and no need of the directory the
 * library resolves: the ABI of its port carries the cwd of the task and its
 * programs use the paths of the shell. The resolver is the library of a
 * program of x86-64, where a program has a directory of its own.
 */
#ifdef MOX_ARCH_XTENSA
#define resolve_path(path) (path)
#else
#define resolve_path(path) __ulibc_path(path)
#endif
#include <stdint.h>

#ifdef MOX_ARCH_XTENSA
/* The pointer _start was entered with (src/start.c). */
extern void *g_mox_door;
#endif

/*
 * A call answers a negative number and the library keeps its magnitude in
 * `errno`: a portable library calls `read` and then reads `errno` without a
 * second call, which is the shape zlib and the sockets of a program expect.
 */
long __ulibc_result(long result)
{
    if (result < 0) {
        errno = (int)-result;
    }

    return result;
}

long __ulibc_syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5)
{
#ifdef MOX_ARCH_XTENSA
    /*
     * The board has no instruction of a system call (ADR 0018 §3a): the port
     * passes the address of its door to _start, and the door is an ordinary
     * function of the kernel with the shape of ksys_call(). The numbers and
     * the arguments are the same as on x86-64; only the crossing differs.
     */
    typedef long (*mox_door_fn)(long number, long a0, long a1, long a2, long a3, long a4, long a5);
    mox_door_fn door = (mox_door_fn)g_mox_door;

    return door(number, a0, a1, a2, a3, a4, a5);
#else
    register long rax __asm__("rax") = number;
    register long rdi __asm__("rdi") = a0;
    register long rsi __asm__("rsi") = a1;
    register long rdx __asm__("rdx") = a2;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;

    /*
     * The instruction of the fast door. It keeps the return address in RCX
     * and the flags in R11 - the port picks both out of the frame of the
     * call - so the two are clobbered here and named as such, which is what
     * keeps the compiler from holding a value of the program in either of
     * them across the call.
     */
    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");

    return rax;
#endif /* MOX_ARCH_XTENSA */
}

/*
 * The numbers of the console, and a descriptor that must not take one of
 * them.
 *
 * The rule of this system is Linux's: descriptors 1 and 2 are the console
 * **while they are not open as a file** (the port writes them to the console
 * itself). A program at the prompt has no terminal of its own behind them, so
 * `pipe()` - the two lowest free numbers - and a bare `open()` hand out 1 and
 * 2 first, and a program of POSIX that then writes its answer "to standard
 * output" writes into its own file while the person at the console sees
 * nothing (`tee` of sbase does exactly that). This call is the one place the
 * library answers it: a new descriptor that lands on 1 or 2 moves to the
 * lowest free number above them, and the console slot is left to the
 * fallback. A caller that *asks* for 1 or 2 - `dup2(fd, 1)` of a redirect -
 * is obeyed, because that is the caller's decision and not a number a call
 * chose.
 *
 * Whether a number of the table is free is the question `fstat` answers: a
 * read of a descriptor that is open for writing answers `EBADF` as a closed
 * one does (`pipe()` of this system answers a written end and a read end of
 * one object), while the record of an open descriptor is answered for every
 * kind of it - a pipe is `ESPIPE` and an open file is a stat. The probe is the
 * raw door, so the caller's `errno` is not touched.
 */
static int descriptor_free(int fd)
{
    struct vfs_statx info;
    long rc = __ulibc_syscall6(KSYS_FSTAT, fd, (long)(uintptr_t)&info, 0, 0, 0, 0);

    return rc == -EBADF;
}

int __ulibc_fd_away(int fd, int other)
{
    int candidate;

    if (fd != ULIBC_STDOUT && fd != ULIBC_STDERR) {
        return fd;
    }

    for (candidate = ULIBC_STDOUT + 1; candidate < TASK_MAX_FDS; candidate++) {
        if (candidate == other) {
            continue;
        }
        if (!descriptor_free(candidate)) {
            continue;
        }
        if (__ulibc_syscall6(KSYS_DUP2, fd, candidate, 0, 0, 0, 0) < 0) {
            return fd;
        }
        (void)__ulibc_syscall6(KSYS_CLOSE, fd, 0, 0, 0, 0, 0);

        return candidate;
    }

    /* Every number above the console is taken: the descriptor keeps the one
     * the kernel chose, and a program with the whole table in use has no room
     * for another descriptor anyway. */
    return fd;
}

/* The flags of the library to the flags of the call. The two sets are not the
 * same numbers: POSIX puts O_CREAT at 0100, the ABI of this kernel at 0x0100,
 * and the library is the one place where the two meet. */
static int open_flags(int flags)
{
    int out = 0;
    int access = flags & 3;

    if (access == ULIBC_O_WRONLY) {
        out |= KSYS_O_WRONLY;
    } else if (access == ULIBC_O_RDWR) {
        out |= KSYS_O_RDWR;
    }

    if (flags & ULIBC_O_CREAT) {
        out |= KSYS_O_CREAT;
    }
    if (flags & ULIBC_O_EXCL) {
        out |= KSYS_O_EXCL;
    }
    if (flags & ULIBC_O_TRUNC) {
        out |= KSYS_O_TRUNC;
    }
    if (flags & ULIBC_O_APPEND) {
        out |= KSYS_O_APPEND;
    }
    if (flags & ULIBC_O_DIRECTORY) {
        out |= KSYS_O_DIRECTORY;
    }

    return out;
}

int open(const char *path, int flags, ...)
{
    const char *resolved = resolve_path(path);

    /* The mode of POSIX is accepted and ignored, as in the library of the
     * kernel: a new file gets the mode of the design and chmod changes it. */
    if (resolved == NULL) {
        return -1;
    }

    {
        long fd = __ulibc_result(
            __ulibc_syscall6(KSYS_OPEN, (long)(uintptr_t)resolved, open_flags(flags), 0, 0, 0, 0));
        int number;

        if (fd < 0) {
            return (int)fd;
        }

        /* 1 and 2 are the console of the system while they are not open as a
         * file, and a file that took one of them would receive what the
         * program writes to its console (`__ulibc_fd_away` above). */
        number = __ulibc_fd_away((int)fd, -1);

#ifndef MOX_ARCH_XTENSA
        if ((flags & ULIBC_O_DIRECTORY) != 0) {
            __ulibc_dir_remember(number, resolved);
        }
#endif

        return number;
    }
}

int close(int fd)
{
    int rc = (int)__ulibc_result(__ulibc_syscall6(KSYS_CLOSE, fd, 0, 0, 0, 0, 0));

#ifndef MOX_ARCH_XTENSA
    if (rc == 0) {
        __ulibc_dir_forget(fd);
    }
#endif

    return rc;
}

long read(int fd, void *buf, size_t count)
{
    return __ulibc_result(
        __ulibc_syscall6(KSYS_READ, fd, (long)(uintptr_t)buf, (long)count, 0, 0, 0));
}

long write(int fd, const void *buf, size_t count)
{
    return __ulibc_result(
        __ulibc_syscall6(KSYS_WRITE, fd, (long)(uintptr_t)buf, (long)count, 0, 0, 0));
}

long lseek(int fd, long offset, int whence)
{
    return __ulibc_result(__ulibc_syscall6(KSYS_LSEEK, fd, offset, whence, 0, 0, 0));
}

int ftruncate(int fd, long long size)
{
    long rc;

    /* A size the register of the machine cannot carry is not cut to fit. */
    if ((long long)(long)size != size) {
        errno = E2BIG;
        return -1;
    }

    rc = __ulibc_result(__ulibc_syscall6(KSYS_FTRUNCATE, fd, (long)size, 0, 0, 0, 0));

    /* The shape of POSIX, like rename(): a program of somebody else calls
     * the function and tests its answer against zero. */
    return (rc < 0) ? -1 : 0;
}

long ulibc_readdir(int fd, struct vfs_dirent *entries, size_t count)
{
    return __ulibc_result(
        __ulibc_syscall6(KSYS_READDIR, fd, (long)(uintptr_t)entries, (long)count, 0, 0, 0));
}

int ulibc_stat_call(const char *path, struct vfs_stat *info)
{
    const char *resolved = resolve_path(path);

    if (resolved == NULL) {
        return -1;
    }

    return (int)__ulibc_result(
        __ulibc_syscall6(KSYS_STAT, (long)(uintptr_t)resolved, (long)(uintptr_t)info, 0, 0, 0, 0));
}

int mkdir(const char *path, ...)
{
    const char *resolved = resolve_path(path);

    if (resolved == NULL) {
        return -1;
    }

    return (int)__ulibc_result(
        __ulibc_syscall6(KSYS_MKDIR, (long)(uintptr_t)resolved, 0, 0, 0, 0, 0));
}

int unlink(const char *path)
{
    const char *resolved = resolve_path(path);

    if (resolved == NULL) {
        return -1;
    }

    return (int)__ulibc_result(
        __ulibc_syscall6(KSYS_UNLINK, (long)(uintptr_t)resolved, 0, 0, 0, 0, 0));
}

/*
 * The rename of a path inside one filesystem, in the shape of POSIX: 0 when
 * it happened and -1 with `errno` when it did not. The library of Lua and a
 * source of a Unix-like system write `if (rename(a, b) != 0)`, so the two
 * calls that the C library promises in that shape are the ones here; the
 * other wrappers keep the raw answer of the ABI.
 */
int rename(const char *from, const char *to)
{
    /* The two names go through the same resolution as `open` and `stat`: a
     * program of this machine has a current directory of its own and the
     * kernel has no notion of one, so a relative name is turned into an
     * absolute one in the library. Without it `rename("a", "b")` answered
     * EINVAL while `open("a", ...)` worked, and a program that saves through
     * a temporary name lost the file. */
    const char *source = __ulibc_path(from);
    const char *target;
    long rc;

    if (source == NULL) {
        return -1;
    }

    target = __ulibc_path(to);
    if (target == NULL) {
        return -1;
    }

    rc =
        __ulibc_syscall6(KSYS_RENAME, (long)(uintptr_t)source, (long)(uintptr_t)target, 0, 0, 0, 0);

    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }

    return 0;
}

sighandler_t signal(int number, sighandler_t handler)
{
    long rc = __ulibc_syscall6(KSYS_SIGNAL, number, (long)(uintptr_t)handler, 0, 0, 0, 0);

    if (rc < 0) {
        errno = (int)-rc;
        return SIG_ERR;
    }

    return (sighandler_t)(uintptr_t)rc;
}

int raise(int number)
{
    long rc = __ulibc_syscall6(KSYS_KILL, getpid(), number, 0, 0, 0, 0);

    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }

    return 0;
}

/*
 * The system has no terminal of a program behind a descriptor: the console of
 * the port is an output, and a read of the descriptor of the input of a
 * program answers EBADF until the shell passes one (`docs/debts.md`). A
 * library that asks whether it talks to a person is told the truth.
 */
int isatty(int fd)
{
    (void)fd;

    return 0;
}

/*
 * The directory of the program, in the buffer of the caller. The task keeps
 * it and the call is the answer: `chdir` moves the record itself (number 48),
 * so a copy in the library could only be a directory of a moment that has
 * passed - the bug that made a child of `execve` start somewhere else.
 */
char *getcwd(char *buf, size_t size)
{
    long rc;

    if (buf == NULL) {
        errno = EINVAL;
        return NULL;
    }

    rc = __ulibc_syscall6(KSYS_GETCWD, (long)(uintptr_t)buf, (long)size, 0, 0, 0, 0);
    if (rc < 0) {
        /* A buffer that cannot hold the path is the ERANGE of POSIX and not
         * the E2BIG the edge of the call uses for a buffer that is too big
         * for a transfer. */
        errno = (rc == -E2BIG) ? ERANGE : (int)-rc;
        return NULL;
    }

    return buf;
}

int getpid(void)
{
    return (int)__ulibc_result(__ulibc_syscall6(KSYS_GETPID, 0, 0, 0, 0, 0, 0));
}

long uptime(void)
{
    return __ulibc_result(__ulibc_syscall6(KSYS_UPTIME, 0, 0, 0, 0, 0, 0));
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    const char *resolved = resolve_path(path);

    if (resolved == NULL) {
        return -1;
    }

    return (int)__ulibc_result(__ulibc_syscall6(KSYS_EXECVE, (long)(uintptr_t)resolved,
                                                (long)(uintptr_t)argv, (long)(uintptr_t)envp, 0, 0,
                                                0));
}

#ifndef MOX_ARCH_XTENSA
/* The handlers of atexit, run by `exit` before the task goes away. */
void __ulibc_atexit_run(void);
#endif

void exit(int code)
{
#ifndef MOX_ARCH_XTENSA
    __ulibc_atexit_run();
#endif
    (void)__ulibc_syscall6(KSYS_EXIT, code, 0, 0, 0, 0, 0);

    /* The call does not come back. The loop is what tells the compiler that
     * the function really never returns, and it keeps the program from
     * running off the end of it if the kernel ever did. */
    for (;;) {
    }
}
