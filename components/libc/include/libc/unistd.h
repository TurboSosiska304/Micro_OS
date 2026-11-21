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
    uint64_t size;
    uint32_t mode;
    uint32_t owner;
    uint64_t mtime;
    uint64_t ctime;
};

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
int microos_statx(const char *path, struct microos_statx *out);
int microos_execve(const char *path, char *const argv[]);

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

#endif /* MICRO_OS_LIBC_UNISTD_H */
