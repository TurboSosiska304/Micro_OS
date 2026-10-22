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

/* The flags of the open, the numbers of POSIX. */
#define MICROOS_O_RDONLY 0
#define MICROOS_O_WRONLY 1
#define MICROOS_O_RDWR 2
#define MICROOS_O_CREAT 0100
#define MICROOS_O_TRUNC 01000
#define MICROOS_O_APPEND 02000

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
long microos_lseek(int fd, long offset, int whence);
int microos_unlink(const char *path);
int microos_mkdir(const char *path);
int microos_waitpid(int pid, int *status, int flags);
int microos_getpid(void);
int microos_getppid(void);
int microos_getcwd(char *buf, size_t size);
unsigned microos_sleep(unsigned ms);
int microos_yield(void);
void microos_exit(int code);

/* The code of the last error, the numbers of POSIX. */
int microos_errno(void);

/*
 * Remember the code of the last error. The library of the system sets
 * errno this way, and a program that checks it after a call of the
 * library sees the number of POSIX.
 */
void microos_set_errno(int code);

/* The text of an error code, the texts of POSIX. */
const char *microos_strerror(int code);

/*
 * The mode of open (the third argument of POSIX) is accepted and
 * ignored: the filesystem of the board has no rights yet. The access
 * alone has to be a real one, 3 is not a mode.
 */
#ifndef MICROOS_LIBC_KEEP_NAMES
#define open microos_open
#define close microos_close
#define read microos_read
#define write microos_write
#define lseek microos_lseek
#endif /* MICROOS_LIBC_KEEP_NAMES */
#define unlink microos_unlink
#define mkdir microos_mkdir
#define waitpid microos_waitpid
#define getpid microos_getpid
#define getppid microos_getppid
#define getcwd microos_getcwd
#define sleep microos_sleep
#define yield microos_yield
#define exit microos_exit

#endif /* MICRO_OS_LIBC_UNISTD_H */
