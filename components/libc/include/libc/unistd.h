/*
 * Micro OS — the system calls of the C library.
 *
 * The names are the usual ones of POSIX. The functions return -1 and set
 * the error (microos_errno) when something goes wrong, the way a program
 * of Linux expects.
 */
#ifndef MICRO_OS_LIBC_UNISTD_H
#define MICRO_OS_LIBC_UNISTD_H

#include <stddef.h>
#include <stdint.h>

/*
 * The information about an object with the moments of a change, the
 * structure of the statx call (ADR 0013). The times are the seconds of
 * Unix, 64 bits wide; zero means that the filesystem keeps no time.
 */
struct microos_statx {
    uint32_t inode;
    uint8_t type;
    /* The group of the object, zero for a volume or a filesystem without one
     * (ADR 0038). It sits in the room that the alignment of `type` leaves
     * free, so that the answer keeps the 40 bytes of the frozen call and an
     * image of 1.0 still reads the fields it knows. */
    uint16_t group;
    uint64_t size;
    uint32_t mode;
    uint32_t owner;
    uint64_t mtime;
    uint64_t ctime;
};

/*
 * The resources of a limit, the selectors of KSYS_RLIMIT_*. A limit starts
 * at the ceiling of the system and may only be lowered: microos_setlimit()
 * answers -1 with EPERM for a raise. The memory is the bytes of the user
 * blocks a task holds, the CPU time is the milliseconds it spent running,
 * and the descriptors are the open files it may take.
 */
#define MICROOS_RLIMIT_MEMORY 1
#define MICROOS_RLIMIT_CPU 2
#define MICROOS_RLIMIT_FDS 3

/* The three limits in one place, for a caller that prints or checks them
 * together. The calls of the ABI move one value at a time. */
struct microos_rlimit {
    uint64_t memory;
    uint32_t cpu_ms;
    uint32_t fds;
};

int microos_getlimit(int which, uint64_t *value);
int microos_setlimit(int which, uint64_t value);
/* The three limits of the caller, or -1 and microos_errno(). */
int microos_getrlimits(struct microos_rlimit *out);

/* The flags of the open, the numbers of POSIX. */
#define MICROOS_O_RDONLY 0
#define MICROOS_O_WRONLY 1
#define MICROOS_O_RDWR 2
#define MICROOS_O_CREAT 0100
#define MICROOS_O_TRUNC 01000
#define MICROOS_O_APPEND 02000
/*
 * Open a directory for reading its entries. The bit is the one of Linux, so
 * that a program carries its usual flags; the kernel keeps its own smaller
 * bit and the library is where the two meet.
 */
#define MICROOS_O_DIRECTORY 0200000

/* The flags of the waitpid. */
#define MICROOS_WNOHANG 1

/*
 * The signals of the system, the numbers of POSIX. The set is the one of
 * kernel/signal.h: the terminal interrupt, the end of the process, the end
 * of a child and a broken pipe.
 */
#define MICROOS_SIGINT 2
#define MICROOS_SIGPIPE 13
#define MICROOS_SIGTERM 15
#define MICROOS_SIGCHLD 17

typedef void (*microos_signal_fn)(int);

/* The two dispositions of the kernel, and the answer of a refused signal(). */
#define MICROOS_SIG_DFL ((microos_signal_fn)0)
#define MICROOS_SIG_IGN ((microos_signal_fn)1)
#define MICROOS_SIG_ERR ((microos_signal_fn) - 1)

/* The whence of the lseek, the numbers of POSIX. */
#define MICROOS_SEEK_SET 0
#define MICROOS_SEEK_CUR 1
#define MICROOS_SEEK_END 2

int microos_open(const char *path, int flags, ...);
int microos_close(int fd);
long microos_read(int fd, void *buf, size_t count);
long microos_write(int fd, const void *buf, size_t count);
/*
 * The ends of a pipe: fds[0] reads, fds[1] writes. The descriptors are
 * ordinary ones of the task, so dup2 and fork treat them as any other file;
 * a read of a pipe whose writing end is closed is the end of the file (0).
 */
int microos_pipe(int fds[2]);
/* A second descriptor of an open file, POSIX style: the old one first. */
int microos_dup2(int oldfd, int newfd);
long microos_lseek(int fd, long offset, int whence);
int microos_unlink(const char *path);
int microos_mkdir(const char *path);
/*
 * Move an object inside one filesystem (ADR 0024 §5). The name `to` must be
 * free: the call does not replace an object. -1 and EPERM/XDEV/NOTSUP when
 * the volume cannot do it.
 */
int microos_rename(const char *from, const char *to);
int microos_waitpid(int pid, int *status, int flags);
int microos_fork(void);
int microos_getpid(void);
int microos_getppid(void);
int microos_getcwd(char *buf, size_t size);
unsigned microos_sleep(unsigned ms);
int microos_yield(void);
void microos_exit(int code);
int microos_chmod(const char *path, unsigned mode);
int microos_chown(const char *path, unsigned owner);
/*
 * The group of an object (ADR 0044). Root gives it to any group; the owner
 * of the object gives it to the group of its own task. A volume without
 * groups - FAT32 - answers ENOTSUP.
 */
int microos_chgrp(const char *path, unsigned group);
int microos_statx(const char *path, struct microos_statx *out);
int microos_execve(const char *path, char *const argv[]);
int microos_kill(int pid, int sig);
/* The previous disposition, or MICROOS_SIG_ERR. */
microos_signal_fn microos_signal(int sig, microos_signal_fn handler);

/* The code of the last error, the numbers of POSIX. */
int microos_errno(void);

/*
 * Remember the code of the last error. The library of the system sets
 * errno this way, and a program that checks it after a call of the
 * library sees the number of POSIX.
 */
void microos_set_errno(int code);

/* The text of an error code, the texts of POSIX. The code may be given with
 * either sign: microos_errno() and a system call disagree about it. */
const char *microos_strerror(int code);

/*
 * The mode of open (the third argument of POSIX) is accepted and
 * ignored: a new file gets the mode of the design, and chmod is the call
 * that changes it. The access alone has to be a real one, 3 is not a mode.
 */
#ifndef MICROOS_LIBC_KEEP_NAMES
#define open microos_open
#define close microos_close
#define read microos_read
#define write microos_write
#define pipe microos_pipe
#define dup2 microos_dup2
#define lseek microos_lseek
#endif /* MICROOS_LIBC_KEEP_NAMES */
#define unlink microos_unlink
#define mkdir microos_mkdir
#define rename microos_rename
#define waitpid microos_waitpid
#define fork microos_fork
#define getpid microos_getpid
#define getppid microos_getppid
#define getcwd microos_getcwd
#define sleep microos_sleep
#define yield microos_yield
#define exit microos_exit
#define chmod microos_chmod
#define chown microos_chown
#define statx microos_statx
#define execve microos_execve
#define kill microos_kill
#define signal microos_signal
#define getlimit microos_getlimit
#define setlimit microos_setlimit

#endif /* MICRO_OS_LIBC_UNISTD_H */
