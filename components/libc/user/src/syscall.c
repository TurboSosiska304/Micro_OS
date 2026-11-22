/*
 * Micro OS — the door of a program.
 *
 * On x86-64 the kernel has no function a program can call: the program was
 * not compiled into it, and the address of ksys_call() means nothing in ring
 * 3. The one door is the vector 128 of the table of the interrupts, and a
 * call is the number in RAX with the six arguments in RDI, RSI, RDX, R10, R8
 * and R9 (ADR 0006). RCX and R11 are not arguments because the instruction
 * `syscall` of a 64-bit machine keeps them for itself; the one convention
 * serves both.
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
static long answer(long result)
{
    if (result < 0) {
        errno = (int)-result;
    }

    return result;
}

static long syscall6(long number, long a0, long a1, long a2, long a3, long a4, long a5)
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

    __asm__ volatile("int $0x80"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");

    return rax;
#endif /* MOX_ARCH_XTENSA */
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
    /* The mode of POSIX is accepted and ignored, as in the library of the
     * kernel: a new file gets the mode of the design and chmod changes it. */
    return (int)answer(syscall6(KSYS_OPEN, (long)(uintptr_t)path, open_flags(flags), 0, 0, 0, 0));
}

int close(int fd)
{
    return (int)answer(syscall6(KSYS_CLOSE, fd, 0, 0, 0, 0, 0));
}

long read(int fd, void *buf, size_t count)
{
    return answer(syscall6(KSYS_READ, fd, (long)(uintptr_t)buf, (long)count, 0, 0, 0));
}

long write(int fd, const void *buf, size_t count)
{
    return answer(syscall6(KSYS_WRITE, fd, (long)(uintptr_t)buf, (long)count, 0, 0, 0));
}

long lseek(int fd, long offset, int whence)
{
    return answer(syscall6(KSYS_LSEEK, fd, offset, whence, 0, 0, 0));
}

long readdir(int fd, struct vfs_dirent *entries, size_t count)
{
    return answer(syscall6(KSYS_READDIR, fd, (long)(uintptr_t)entries, (long)count, 0, 0, 0));
}

int stat(const char *path, struct vfs_stat *info)
{
    return (int)answer(
        syscall6(KSYS_STAT, (long)(uintptr_t)path, (long)(uintptr_t)info, 0, 0, 0, 0));
}

int mkdir(const char *path)
{
    return (int)answer(syscall6(KSYS_MKDIR, (long)(uintptr_t)path, 0, 0, 0, 0, 0));
}

int unlink(const char *path)
{
    return (int)answer(syscall6(KSYS_UNLINK, (long)(uintptr_t)path, 0, 0, 0, 0, 0));
}

char *getcwd(char *buf, size_t size)
{
    long rc = answer(syscall6(KSYS_GETCWD, (long)(uintptr_t)buf, (long)size, 0, 0, 0, 0));

    return (rc < 0) ? NULL : buf;
}

int getpid(void)
{
    return (int)answer(syscall6(KSYS_GETPID, 0, 0, 0, 0, 0, 0));
}

long uptime(void)
{
    return answer(syscall6(KSYS_UPTIME, 0, 0, 0, 0, 0, 0));
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)answer(syscall6(KSYS_EXECVE, (long)(uintptr_t)path, (long)(uintptr_t)argv,
                                (long)(uintptr_t)envp, 0, 0, 0));
}

void exit(int code)
{
    (void)syscall6(KSYS_EXIT, code, 0, 0, 0, 0, 0);

    /* The call does not come back. The loop is what tells the compiler that
     * the function really never returns, and it keeps the program from
     * running off the end of it if the kernel ever did. */
    for (;;) {
    }
}
