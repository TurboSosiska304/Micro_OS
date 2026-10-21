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

#include "kernel/task.h"
#include "kernel/types.h"

/*
 * The system calls. The numbers are frozen: a new call gets the next free
 * number, an old one keeps its number forever.
 */
#define KSYS_EXIT 0    /* exit(code) */
#define KSYS_READ 1    /* read(fd, buf, count) */
#define KSYS_WRITE 2   /* write(fd, buf, count) */
#define KSYS_OPEN 3    /* open(path, flags) */
#define KSYS_CLOSE 4   /* close(fd) */
#define KSYS_STAT 5    /* stat(path, buf) */
#define KSYS_LSEEK 6   /* lseek(fd, offset, whence) */
#define KSYS_UNLINK 7  /* unlink(path) */
#define KSYS_MKDIR 8   /* mkdir(path) */
#define KSYS_READDIR 9 /* readdir(fd, buf, count) */
#define KSYS_GETPID 10 /* getpid() */
#define KSYS_GETCWD 11 /* getcwd(buf, size) */
#define KSYS_IOCTL 12  /* ioctl(fd, cmd, arg) */
#define KSYS_YIELD 13  /* yield() */
#define KSYS_SLEEP 14  /* sleep(ms) */
#define KSYS_UPTIME 15 /* uptime() */
#define KSYS_REBOOT 16 /* reboot() */
#define KSYS_COUNT 17  /* the number of the system calls */

/* The flags of the open call, the same as the ones of the VFS. */
#define KSYS_O_RDONLY 0x0000
#define KSYS_O_WRONLY 0x0001
#define KSYS_O_RDWR 0x0002
#define KSYS_O_CREAT 0x0100
#define KSYS_O_TRUNC 0x0200
#define KSYS_O_APPEND 0x0400

/* The whence of the lseek. */
#define KSYS_SEEK_SET 0
#define KSYS_SEEK_CUR 1
#define KSYS_SEEK_END 2

/*
 * The handler of a system call. The arguments are the raw values of the
 * registers, the pointers are passed as they are.
 *
 * The arguments are 64 bits wide, so that the ABI is the same on a 32 bit
 * target and on a 64 bit host: a pointer must not be cut in half.
 */
typedef k_ssize_t (*ksys_handler_fn)(k_u64_t a0, k_u64_t a1, k_u64_t a2, k_u64_t a3, k_u64_t a4,
                                     k_u64_t a5);

/* The description of a system call, for the documentation and the tests. */
struct ksys_entry {
    k_u32_t number;
    const char *name;
    ksys_handler_fn handler;
};

/* Register a system call. The number of the call is its contract. */
int ksys_register(k_u32_t number, const char *name, ksys_handler_fn handler);

/* Call a system call. */
k_ssize_t ksys_call(k_u32_t number, k_u64_t a0, k_u64_t a1, k_u64_t a2, k_u64_t a3, k_u64_t a4,
                    k_u64_t a5);

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
k_ssize_t ksys_lseek(int fd, k_ssize_t offset, int whence);
int ksys_unlink(const char *path);
int ksys_mkdir(const char *path);
k_ssize_t ksys_readdir(int fd, void *buf, k_size_t count);
k_u32_t ksys_getpid(void);
int ksys_getcwd(char *buf, k_size_t size);
int ksys_yield(void);
int ksys_sleep(k_u64_t ms);
k_u64_t ksys_uptime(void);
int ksys_reboot(void);

#endif /* MICRO_OS_KERNEL_KSYS_H */
