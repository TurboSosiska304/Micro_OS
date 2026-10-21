/*
 * Micro OS — the error codes of the kernel.
 *
 * The same values as the POSIX ones, so that a program does not have to
 * learn a new language. The result of a system call is negative when
 * something went wrong.
 */
#ifndef MICRO_OS_KERNEL_ERRNO_H
#define MICRO_OS_KERNEL_ERRNO_H

#define KERR_NONE 0
#define KERR_PERM (-1)   /* operation not permitted */
#define KERR_NOENT (-2)  /* no such file or directory */
#define KERR_EXIST (-3)  /* file exists */
#define KERR_NOSPC (-4)  /* no space left on device */
#define KERR_IO (-5)     /* input/output error */
#define KERR_INVAL (-6)  /* invalid argument */
#define KERR_NOTDIR (-7) /* not a directory */
#define KERR_ISDIR (-8)  /* is a directory */
#define KERR_NOTEMPTY (-9)
#define KERR_TOOBIG (-10)
#define KERR_BADF (-11)  /* bad file descriptor */
#define KERR_NOSYS (-12) /* the system call does not exist */
#define KERR_AGAIN (-13) /* try again later */

#endif /* MICRO_OS_KERNEL_ERRNO_H */
