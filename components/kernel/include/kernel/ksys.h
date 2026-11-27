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
#define KSYS_EXIT 0           /* exit(code) */
#define KSYS_READ 1           /* read(fd, buf, count) */
#define KSYS_WRITE 2          /* write(fd, buf, count) */
#define KSYS_OPEN 3           /* open(path, flags) */
#define KSYS_CLOSE 4          /* close(fd) */
#define KSYS_STAT 5           /* stat(path, buf) */
#define KSYS_LSEEK 6          /* lseek(fd, offset, whence) */
#define KSYS_UNLINK 7         /* unlink(path) */
#define KSYS_MKDIR 8          /* mkdir(path) */
#define KSYS_READDIR 9        /* readdir(fd, buf, count) */
#define KSYS_GETPID 10        /* getpid() */
#define KSYS_GETCWD 11        /* getcwd(buf, size) */
#define KSYS_IOCTL 12         /* ioctl(fd, cmd, arg) */
#define KSYS_YIELD 13         /* yield() */
#define KSYS_SLEEP 14         /* sleep(ms) */
#define KSYS_UPTIME 15        /* uptime() */
#define KSYS_REBOOT 16        /* reboot() */
#define KSYS_FORK 17          /* fork() */
#define KSYS_GETPPID 18       /* getppid() */
#define KSYS_WAITPID 19       /* waitpid(pid, status) */
#define KSYS_MALLOC 20        /* malloc(size) */
#define KSYS_FREE 21          /* free(ptr) */
#define KSYS_REALLOC 22       /* realloc(ptr, size) */
#define KSYS_CHMOD 23         /* chmod(path, mode) */
#define KSYS_CHOWN 24         /* chown(path, owner) */
#define KSYS_EXECVE 25        /* execve(path, argv, envp) */
#define KSYS_STATX 26         /* statx(path, buf) */
#define KSYS_PIPE 27          /* pipe(fds) */
#define KSYS_DUP2 28          /* dup2(oldfd, newfd) */
#define KSYS_KILL 29          /* kill(pid, sig) */
#define KSYS_SIGNAL 30        /* signal(sig, handler) -> the old handler */
#define KSYS_SIGRETURN 31     /* the restorer of a signal frame, for the port */
#define KSYS_GETLIMIT 32      /* getlimit(which, value) */
#define KSYS_SETLIMIT 33      /* setlimit(which, value) */
#define KSYS_RENAME 34        /* rename(from, to) */
#define KSYS_FTRUNCATE 35     /* ftruncate(fd, size) */
#define KSYS_GETUID 36        /* getuid() */
#define KSYS_GETEUID 37       /* geteuid() */
#define KSYS_GETGID 38        /* getgid() */
#define KSYS_SETUID 39        /* setuid(uid) */
#define KSYS_SETGID 40        /* setgid(gid) */
#define KSYS_RMDIR 41         /* rmdir(path) */
#define KSYS_FSTAT 42         /* fstat(fd, buf) */
#define KSYS_POLL 43          /* poll(fds, nfds, timeout) */
#define KSYS_MMAP 44          /* mmap(addr, len, prot, flags, fd, offset) */
#define KSYS_MUNMAP 45        /* munmap(addr, len) */
#define KSYS_MSYNC 46         /* msync(addr, len) */
#define KSYS_BRK 47           /* brk(addr) */
#define KSYS_CHDIR 48         /* chdir(path) */
#define KSYS_CLOCK_GETTIME 49 /* clock_gettime(clock, buf) */
#define KSYS_GETRUSAGE 50     /* getrusage(buf) */
#define KSYS_CHGRP 51         /* chgrp(path, group) */
#define KSYS_GETPGID 52       /* getpgid(pid) -> pgid */
#define KSYS_SETPGID 53       /* setpgid(pid, pgid) */
#define KSYS_FSYNC 54         /* fsync(fd) */
#define KSYS_COUNT 55         /* the number of the system calls */

/*
 * The resources of a limit. One number names one resource and the calls move
 * its value as a 64-bit word; there is no structure on the wire, because the
 * three values have three different widths and a pointer of the caller would
 * have to be checked and copied for nothing. The library composes
 * `struct microos_rlimit` from three calls for the callers that want the set
 * in one place.
 *
 * The limits of a task start at the maximum of the system and may only be
 * lowered: the ceiling belongs to the machine, and a program must not raise
 * it, root included (the rule of POSIX without the privilege of the limit,
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
/* With O_CREAT the open fails with EEXIST instead of opening a name that is
 * already there (the exclusive create of POSIX). */
#define KSYS_O_EXCL 0x1000

/* The bits of the access: read, write, or both. */
#define KSYS_O_ACCMODE 0x0003

/* Every bit an open may carry: a flag outside this set is not of the
 * contract, and the call is refused instead of being guessed at. */
#define KSYS_O_ALLOWED                                                                             \
    (KSYS_O_ACCMODE | KSYS_O_CREAT | KSYS_O_TRUNC | KSYS_O_APPEND | KSYS_O_DIRECTORY | KSYS_O_EXCL)

/* The flags of the waitpid. */
#define KSYS_WNOHANG 1 /* do not wait, answer 0 when the child is not ready */

/*
 * The mode, the owner and the group of an object, as the edge of the system
 * accepts them: the mode is the nine bits of the rwx triads, the owner is a
 * uid of fifteen bits and the group a gid of the same width, which is what
 * every filesystem of the system keeps for the three numbers (ADR 0005,
 * ADR 0044). A wider number is refused at the door instead of being cut to
 * fit the record.
 *
 * The root of the system has the uid zero: it passes the triads, changes
 * the owner and the group of an object, mounts a volume and signals any
 * task. A task of another uid is the owner of what it owns, reads the triad
 * of the group when its gid is the group of the object, and the "others" of
 * everything else.
 */
#define KSYS_MODE_MASK 0777u
#define KSYS_UID 0u
#define KSYS_UID_MAX 0x7FFFu
#define KSYS_GID_MAX KSYS_UID_MAX

/* The whence of the lseek. */
#define KSYS_SEEK_SET 0
#define KSYS_SEEK_CUR 1
#define KSYS_SEEK_END 2

/*
 * `poll` (number 43): one entry per descriptor the caller watches, the
 * events it is interested in and the events that are true now.
 *
 * The layout is eight bytes on both machines of the system: the descriptor is
 * 32 bits, the two masks are 16 each, and no field has a pointer in it. The
 * numbers of the bits are the ones of Linux, so a program that knows one
 * contract knows the other, and the set is the three bits this system can
 * answer: a read would not wait, a write would not wait, and the object is in
 * an error state (the reading end of a pipe is gone, or the number is not an
 * open descriptor).
 *
 * A zero count is a sleep: the call waits for the timeout and answers 0. A
 * timeout of -1 waits until an object of the list changes its state. The wait
 * is the scheduler's - the task leaves the run queue - so a program of the
 * console that waits for a pipe costs no processor time.
 */
struct ksys_pollfd {
    k_u32_t fd;      /* the descriptor, or a negative number to be ignored */
    k_u16_t events;  /* what the caller asks about */
    k_u16_t revents; /* what is true, written by the call */
};

#define KSYS_POLLIN 0x0001  /* a read would not wait */
#define KSYS_POLLOUT 0x0004 /* a write would not wait */
#define KSYS_POLLERR 0x0008 /* the object is gone or the number is not open */

/* The entries one call may watch: the table of the descriptors of a task is
 * sixteen slots, so a list longer than that names a descriptor twice at
 * best. */
#define KSYS_POLL_MAX 16

/*
 * The memory a program maps (numbers 44 to 47).
 *
 * `mmap(addr, len, prot, flags, fd, offset)` names a range of the memory of
 * the program. `addr` is a hint (0 for "anywhere"), `len` is a whole number of
 * pages, `offset` is a page-aligned offset into the file behind `fd`, and the
 * answer is the address the range got. A range that is not backed by a file
 * is `KSYS_MAP_ANONYMOUS` and reads as zeros.
 *
 * The pages are made when the program touches them: a mapping of a file of
 * 64 MiB costs one page of memory, not 64 MiB, and the bytes of the page come
 * from the filesystem on the fault. A write of a `KSYS_MAP_PRIVATE` page is a
 * copy of that page; a write of a `KSYS_MAP_SHARED` one changes the file, and
 * `msync` writes the pages that were changed back to it.
 *
 * `munmap(addr, len)` gives the range back and `msync(addr, len)` writes the
 * shared pages of it to their file. `brk(addr)` moves the end of the heap and
 * answers the new end; a heap that would cross a mapping or the stack of the
 * program is `ENOMEM` and the break stays where it was. A machine without an
 * address space of a program answers `ENOSYS` to all four, as it does to
 * `execve`.
 */
#define KSYS_PROT_READ 0x01u
#define KSYS_PROT_WRITE 0x02u
#define KSYS_PROT_EXEC 0x04u

#define KSYS_MAP_SHARED 0x01u
#define KSYS_MAP_PRIVATE 0x02u
#define KSYS_MAP_ANONYMOUS 0x04u

/* The page of the mappings: a length and an offset are multiples of it. */
#define KSYS_PAGE_SIZE 4096u

/*
 * The directory of the task and the clock of the machine (numbers 48 and 49).
 *
 * `chdir` moves the working directory of the calling task: the kernel keeps
 * one path for the task, `execve` carries it into a program and `fork` copies
 * it, so a child starts where its parent moved to. The path has to be a
 * directory of the VFS, and a path that does not fit the record of a task
 * (TASK_CWD_MAX, the same 127 characters a call accepts) is ENAMETOOLONG.
 *
 * `clock_gettime` is the wall clock and the monotonic one in the shape of
 * POSIX. The wire is two signed 64-bit words on both machines, because the
 * seconds of Unix are the same number on the board and on x86-64; the library
 * of a 32-bit target translates them into its own `struct timespec`.
 */
#define KSYS_CLOCK_REALTIME 0  /* the seconds since 1970, the wall clock */
#define KSYS_CLOCK_MONOTONIC 1 /* the milliseconds since the boot, as uptime */

struct ksys_timespec {
    k_i64_t tv_sec;  /* seconds; since 1970 for CLOCK_REALTIME */
    k_i64_t tv_nsec; /* the fraction of the second, 0 to 999999999 */
};

/*
 * The use of the processor by the calling task (number 50). `utime_ms` is the
 * time the task spent running since it was created and `stime_ms` the part of
 * it the kernel spent on the task's behalf. This system makes no boundary
 * between the two on a machine of one privilege level - a call is a call -
 * and the scheduler counts one number for both (kernel/task.h): the whole of
 * it is the user time and `stime_ms` is zero today.
 */
struct ksys_rusage {
    k_u64_t utime_ms;
    k_u64_t stime_ms;
};

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
/*
 * The information about the object behind an open descriptor. The call is
 * the `statx` of the name the descriptor was opened with: this system has no
 * separate table of the names of open files, so an object that was renamed
 * after its open answers for its old name.
 */
int ksys_fstat(int fd, void *buf);
k_ssize_t ksys_lseek(int fd, k_ssize_t offset, int whence);
int ksys_unlink(const char *path);
int ksys_mkdir(const char *path);
/* Remove an empty directory (POSIX rmdir). */
int ksys_rmdir(const char *path);
/* The same object under another name, inside one filesystem (ADR 0024 §5). */
int ksys_rename(const char *from, const char *to);
/*
 * Cut an open file to `size` bytes, the `ftruncate` of POSIX. The
 * descriptor has to be open for writing, the size has to fit the volume,
 * and a filesystem without the operation answers KERR_NOTSUP. The width of
 * the size follows the machine, like every argument of a call: 64 bits on
 * the host and x86-64, 32 on the board.
 */
int ksys_ftruncate(int fd, k_u64_t size);
k_ssize_t ksys_readdir(int fd, void *buf, k_size_t count);
k_u32_t ksys_getpid(void);
/*
 * The credentials of the caller. geteuid answers the same number as getuid:
 * this kernel keeps one uid and not the real/effective pair, because the
 * image of a program has no setuid bit and nothing moves one of the two
 * alone. setuid is root giving up root (to any uid) or a task of another uid
 * naming itself (a no-op); any other change is KERR_PERM. setgid follows the
 * same rule, and the gid it stores decides no permission yet.
 */
k_u32_t ksys_getuid(void);
k_u32_t ksys_geteuid(void);
k_u32_t ksys_getgid(void);
int ksys_setuid(k_u32_t uid);
int ksys_setgid(k_u32_t gid);
int ksys_getcwd(char *buf, k_size_t size);
int ksys_yield(void);
int ksys_sleep(ksys_arg_t ms);
k_u64_t ksys_uptime(void);
int ksys_reboot(void);
int ksys_fork(void);
int ksys_getppid(void);
k_i32_t ksys_getpgid(k_u32_t pid);
int ksys_setpgid(k_u32_t pid, k_u32_t pgid);
int ksys_waitpid(int pid, int *status, int flags);
void *ksys_malloc(k_size_t size);
void ksys_free(void *ptr);
void *ksys_realloc(void *ptr, k_size_t size);
int ksys_chmod(const char *path, k_u32_t mode);
int ksys_chown(const char *path, k_u32_t owner);
int ksys_chgrp(const char *path, k_u32_t group);
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

/*
 * Wait for one of the descriptors of `fds` to be ready: the count of the
 * entries whose revents is not zero, 0 when the timeout passed, or an error.
 * `nfds` of zero is a sleep of `timeout_ms` milliseconds, and -1 as the
 * timeout waits without a deadline. A signal that the caller has to answer
 * cuts the wait with EINTR.
 */
int ksys_poll(struct ksys_pollfd *fds, k_u32_t nfds, int timeout_ms);
/* The same call with an environment of the caller. */
int ksys_execve_env(const char *path, char *const argv[], char *const envp[]);

/* The memory of a program: a mapping, its release, the write-back of a shared
 * one and the heap. The addresses are the memory of the program, not of the
 * kernel: a mapping of a file is the file the descriptor names. */
k_ssize_t ksys_mmap(void *addr, k_size_t len, k_u32_t prot, k_u32_t flags, int fd, k_u64_t offset);
int ksys_munmap(void *addr, k_size_t len);
int ksys_msync(void *addr, k_size_t len);
/* The new break, or a negative error (ENOMEM when the heap may not grow). */
k_ssize_t ksys_brk(void *addr);
/*
 * The working directory of the calling task. `chdir` moves it and copies it
 * into the programs the task starts; a name that is not a directory of the
 * VFS is refused and the directory the task had is kept.
 */
int ksys_chdir(const char *path);
/* The moment of one of the two clocks: 0, or a negative error. */
int ksys_clock_gettime(k_u32_t clock, struct ksys_timespec *out);
/* The processor time of the calling task: 0, or a negative error. */
int ksys_getrusage(struct ksys_rusage *out);
/* Bring the data and the metadata of an open file to the medium. */
int ksys_fsync(int fd);
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
