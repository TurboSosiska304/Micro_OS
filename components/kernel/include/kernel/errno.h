/*
 * Micro OS — the error codes of the kernel.
 *
 * The same values as the POSIX ones, so that a program does not have to
 * learn a new language. The result of a system call is negative when
 * something went wrong.
 */
#ifndef MICRO_OS_KERNEL_ERRNO_H
#define MICRO_OS_KERNEL_ERRNO_H

/*
 * The values are the numbers of POSIX, so that a program that knows the
 * usual error codes does not have to learn new ones. The kernel returns
 * them with the minus sign.
 */
#define KERR_NONE 0
#define KERR_PERM (-1)  /* operation not permitted */
#define KERR_NOENT (-2) /* no such file or directory */
/*
 * ESRCH and EEXIST share a number here: the errno table of this system was
 * written before signals, and -3 became "file exists". A pid that names no
 * task is the same number, which is right for the caller that sent a signal
 * and wrong only for the text of the error, so a signal prints its own
 * sentence in the shell.
 */
#define KERR_SRCH (-3)   /* no such process */
#define KERR_EXIST (-3)  /* file exists */
#define KERR_INTR (-4)   /* interrupted by a signal */
#define KERR_IO (-5)     /* input/output error */
#define KERR_TOOBIG (-7) /* argument list too long */
#define KERR_BADF (-9)   /* bad file descriptor */
#define KERR_CHILD (-10) /* no child processes */
#define KERR_AGAIN (-11) /* try again later */
#define KERR_NOMEM (-12) /* out of memory */
#define KERR_FAULT (-14) /* a pointer of the caller is not its memory */
#define KERR_BUSY (-16)  /* the mount point is in use */
#define KERR_NOSYS (-38) /* the system call does not exist */
#define KERR_NOTDIR (-20)
#define KERR_ISDIR (-21)
#define KERR_INVAL (-22)
#define KERR_NOSPC (-28)       /* no space left on device */
#define KERR_SPIPE (-29)       /* illegal seek: a pipe has no position */
#define KERR_PIPE (-32)        /* broken pipe: the reading end is closed */
#define KERR_NAMETOOLONG (-36) /* the path is longer than the window */
#define KERR_NOTEMPTY (-39)
#define KERR_NOTSUP (-95) /* operation not supported by the object */

#endif /* MICRO_OS_KERNEL_ERRNO_H */
