/*
 * Micro OS — the ABI of the system calls.
 *
 * The numbers of the system calls are a public contract: they are part of
 * the interface between the kernel and the programs, and they never change.
 * A program that was compiled for one version of the kernel keeps working
 * with the next one.
 *
 * The convention is the one of Linux: the number of the call and six
 * arguments in the registers, the result in the return value. A negative
 * result is an error, the numbers are the negative error codes
 * (see kernel/errno.h).
 */
#ifndef MICRO_OS_KERNEL_KSYS_H
#define MICRO_OS_KERNEL_KSYS_H

#include "kernel/signal.h"
#include "kernel/task.h"
#include "kernel/types.h"

/*
 * The system calls. The numbers are frozen: a new call gets the next free
 * number, an old one keeps its number forever.
 */
#define KSYS_EXIT 0       /* exit(code) */
#define KSYS_READ 1       /* read(fd, buf, count) */
#define KSYS_WRITE 2      /* write(fd, buf, count) */
#define KSYS_OPEN 3       /* open(path, flags) */
#define KSYS_CLOSE 4      /* close(fd) */
#define KSYS_STAT 5       /* stat(path, buf) */
#define KSYS_LSEEK 6      /* lseek(fd, offset, whence) */
#define KSYS_UNLINK 7     /* unlink(path) */
#define KSYS_MKDIR 8      /* mkdir(path) */
#define KSYS_READDIR 9    /* readdir(fd, buf, count) */
#define KSYS_GETPID 10    /* getpid() */
#define KSYS_GETCWD 11    /* getcwd(buf, size) */
#define KSYS_IOCTL 12     /* ioctl(fd, cmd, arg) */
#define KSYS_YIELD 13     /* yield() */
#define KSYS_SLEEP 14     /* sleep(ms) */
#define KSYS_UPTIME 15    /* uptime() */
#define KSYS_REBOOT 16    /* reboot() */
#define KSYS_FORK 17      /* fork() */
#define KSYS_GETPPID 18   /* getppid() */
#define KSYS_WAITPID 19   /* waitpid(pid, status) */
#define KSYS_MALLOC 20    /* malloc(size) */
#define KSYS_FREE 21      /* free(ptr) */
#define KSYS_REALLOC 22   /* realloc(ptr, size) */
#define KSYS_CHMOD 23     /* chmod(path, mode) */
#define KSYS_CHOWN 24     /* chown(path, owner) */
#define KSYS_EXECVE 25    /* execve(path, argv, envp) */
#define KSYS_STATX 26     /* statx(path, buf) */
#define KSYS_PIPE 27      /* pipe(fds) */
#define KSYS_DUP2 28      /* dup2(oldfd, newfd) */
#define KSYS_KILL 29      /* kill(pid, sig) */
#define KSYS_SIGNAL 30    /* signal(sig, handler) -> the old handler */
#define KSYS_SIGRETURN 31 /* the restorer of a signal frame, for the port */
#define KSYS_GETLIMIT 32  /* getlimit(which, value) */
#define KSYS_SETLIMIT 33  /* setlimit(which, value) */
#define KSYS_COUNT 34     /* the number of the system calls */

/*
 * The resources of a limit. One number names one resource and the calls move
 * its value as a 64-bit word; there is no structure on the wire, because the
 * three values have three different widths and a pointer of the caller would
 * have to be checked and copied for nothing. The library composes
 * `struct microos_rlimit` from three calls for the callers that want the set
 * in one place.
 *
 * The limits of a task start at the maximum of the system and may only be
 * lowered: there is one user, uid 0, and a program of that user still must
 * not raise its own ceiling (the rule of POSIX without the privilege of root,
 * like the mode check of ADR 0005). A value above the current limit is
 * EPERM; a resource that is not one of the three is EINVAL.
 */
#define KSYS_RLIMIT_MEMORY 1u /* bytes of the user blocks of the task */
#define KSYS_RLIMIT_CPU 2u    /* milliseconds of processor time */
#define KSYS_RLIMIT_FDS 3u    /* open descriptors of the task */

/* The ceiling of each resource: the limit of a fresh task and the most a
 * task may ever ask of the machine. */
#define KSYS_RLIMIT_MEMORY_MAX TASK_LIMIT_MEMORY_MAX
#define KSYS_RLIMIT_CPU_MAX TASK_LIMIT_CPU_MAX
#define KSYS_RLIMIT_FDS_MAX TASK_LIMIT_FDS_MAX

/*
 * The signals of the ABI. The numbers are the ones of POSIX and the set is
 * the one of the kernel (kernel/signal.h): the terminal interrupt, the end of
 * a process, the end of a child and a broken pipe. The disposition is a
 * pointer of the caller, or one of the two values below.
 */
#define KSYS_SIGINT K_SIGINT
#define KSYS_SIGPIPE K_SIGPIPE
#define KSYS_SIGTERM K_SIGTERM
#define KSYS_SIGCHLD K_SIGCHLD
#define KSYS_SIG_DFL 0
#define KSYS_SIG_IGN 1

/* The flags of the open call, the same as the ones of the VFS. */
#define KSYS_O_RDONLY 0x0000
#define KSYS_O_WRONLY 0x0001
#define KSYS_O_RDWR 0x0002
#define KSYS_O_CREAT 0x0100
#define KSYS_O_TRUNC 0x0200
#define KSYS_O_APPEND 0x0400
/*
 * Open a directory as a directory, which is what readdir needs. The flag
 * goes with O_RDONLY and with nothing else: a directory is read, never
 * written through a descriptor of this system.
 */
#define KSYS_O_DIRECTORY 0x0800

/* The bits of the access: read, write, or both. */
#define KSYS_O_ACCMODE 0x0003

/* Every bit an open may carry: a flag outside this set is not of the
 * contract, and the call is refused instead of being guessed at. */
#define KSYS_O_ALLOWED                                                                             \
    (KSYS_O_ACCMODE | KSYS_O_CREAT | KSYS_O_TRUNC | KSYS_O_APPEND | KSYS_O_DIRECTORY)

/* The flags of the waitpid. */
#define KSYS_WNOHANG 1 /* do not wait, answer 0 when the child is not ready */

/*
 * The mode and the owner of an object, as the edge of the system accepts
 * them: the mode is the nine bits of the rwx triads and the owner is a uid
 * of fifteen bits, which is what every filesystem of the system keeps for
 * the two numbers (ADR 0005). A wider number is refused at the door instead
 * of being cut to fit the record.
 *
 * The system has one user and its uid is zero; the checks of the mode use
 * that number and grant it nothing extra.
 */
#define KSYS_MODE_MASK 0777u
#define KSYS_UID 0u
#define KSYS_UID_MAX 0x7FFFu

/* The whence of the lseek. */
#define KSYS_SEEK_SET 0
#define KSYS_SEEK_CUR 1
#define KSYS_SEEK_END 2

/*
 * The longest path a call accepts, the terminator not counted: a path has
 * to fit the buffer the VFS and every filesystem under it keep for it.
 */
#define KSYS_PATH_MAX (VFS_PATH_MAX - 1)

/*
 * The most bytes one call may touch. The kernel does not copy a buffer of
 * an arbitrary length into its own memory, but it does walk it on behalf of
 * the caller, and a number far above any buffer of the system is a mistake
 * or an attempt to make the kernel read the memory it should not. The
 * internal RAM of the board is about 100 KB, so 64 KB is the largest
 * transfer that can still be a real one.
 */
#define KSYS_MAX_IO 65536u

/*
 * The width of the arguments of a system call.
 *
 * The arguments are the raw values of the registers, so their width is the
 * width of a register of the target: 32 bits on the board (the kernel of
 * the ESP32 works with 32 bit numbers), 64 bits on a 64 bit host. The
 * numbers of the calls are the same everywhere, only the width of the
 * arguments belongs to the target, like in the real syscall interface.
 */
#if UINTPTR_MAX == 0xFFFFFFFFu
typedef k_u32_t ksys_arg_t;
#else
typedef k_u64_t ksys_arg_t;
#endif

/*
 * The handler of a system call. The arguments are the raw values of the
 * registers, the pointers are passed as they are.
 */
typedef k_ssize_t (*ksys_handler_fn)(ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                                     ksys_arg_t a4, ksys_arg_t a5);

/* The description of a system call, for the documentation and the tests. */
struct ksys_entry {
    k_u32_t number;
    const char *name;
    ksys_handler_fn handler;
};

/* Register a system call. The number of the call is its contract. */
int ksys_register(k_u32_t number, const char *name, ksys_handler_fn handler);

/* Call a system call. */
k_ssize_t ksys_call(k_u32_t number, ksys_arg_t a0, ksys_arg_t a1, ksys_arg_t a2, ksys_arg_t a3,
                    ksys_arg_t a4, ksys_arg_t a5);

/* The name of a system call, NULL if there is none. */
const char *ksys_name(k_u32_t number);

/* The number of the registered system calls. */
k_u32_t ksys_count(void);

/* Register the system calls of the kernel. */
void ksys_init(void);

/*
 * The convenient wrappers. They are what the programs use; the arguments
 * are checked here, so that a mistake does not reach the kernel.
 */
k_ssize_t ksys_read(int fd, void *buf, k_size_t count);
k_ssize_t ksys_write(int fd, const void *buf, k_size_t count);
int ksys_open(const char *path, int flags);
int ksys_close(int fd);
int ksys_stat(const char *path, void *buf);
/*
 * The information about an object with the moments of a change. The old
 * stat keeps its structure and its number; this is the second door for a
 * caller that wants the two times (ADR 0013).
 */
int ksys_statx(const char *path, void *buf);
k_ssize_t ksys_lseek(int fd, k_ssize_t offset, int whence);
int ksys_unlink(const char *path);
int ksys_mkdir(const char *path);
k_ssize_t ksys_readdir(int fd, void *buf, k_size_t count);
k_u32_t ksys_getpid(void);
int ksys_getcwd(char *buf, k_size_t size);
int ksys_yield(void);
int ksys_sleep(ksys_arg_t ms);
k_u64_t ksys_uptime(void);
int ksys_reboot(void);
int ksys_fork(void);
int ksys_getppid(void);
int ksys_waitpid(int pid, int *status, int flags);
void *ksys_malloc(k_size_t size);
void ksys_free(void *ptr);
void *ksys_realloc(void *ptr, k_size_t size);
int ksys_chmod(const char *path, k_u32_t mode);
int ksys_chown(const char *path, k_u32_t owner);
/*
 * Make the caller a program. `argv` is its vector of arguments (NULL for
 * none) and `envp` its environment, a vector of "NAME=VALUE" words (NULL for
 * the environment of the system). Both vectors are the memory of the caller
 * and are copied before the file is read; each is at most eight words and 512
 * bytes. The wrapper without an environment is the common case, and it is the
 * one the call had before the environment existed.
 */
int ksys_execve(const char *path, char *const argv[]);
/*
 * The ends of a pipe: fds[0] reads, fds[1] writes. A descriptor of the
 * table of the task each, so dup2 and a fork treat them as any other file;
 * the pipe goes with the last descriptor of either end.
 */
int ksys_pipe(int fds[2]);
/* A second descriptor of an open file, POSIX style. */
int ksys_dup2(int oldfd, int newfd);
/* The same call with an environment of the caller. */
int ksys_execve_env(const char *path, char *const argv[], char *const envp[]);
int ksys_kill(int pid, int sig);
/* The old handler, or K_SIG_ERR when the signal is not one of the system. */
task_signal_fn ksys_signal(int sig, task_signal_fn handler);

/*
 * The limits of the calling task. `which` is one of KSYS_RLIMIT_*, the value
 * is the number of bytes, of milliseconds or of descriptors. getlimit copies
 * the current limit into the place of the caller; setlimit lowers it. A call
 * that would lower the limit of another task is not expressible: the limits
 * are the task's own, as `setrlimit` of POSIX with the resource of the
 * caller.
 */
int ksys_getlimit(k_u32_t which, k_u64_t *value);
int ksys_setlimit(k_u32_t which, k_u64_t value);

#endif /* MICRO_OS_KERNEL_KSYS_H */
