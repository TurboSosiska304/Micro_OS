/*
 * Micro OS — the flags of open for a ported program.
 *
 * The numbers are the POSIX ones and the library maps them to the ABI of the
 * kernel (`KSYS_O_*`). A flag this system does not have (O_EXCL, O_CLOEXEC)
 * is not defined on purpose: a source that checks for it with #ifdef then
 * takes the path of a machine without it, and no call receives a bit the
 * kernel would refuse.
 */
#ifndef MICRO_OS_LIBC_USER_FCNTL_H
#define MICRO_OS_LIBC_USER_FCNTL_H

#include "ulibc.h"

#define O_RDONLY ULIBC_O_RDONLY
#define O_WRONLY ULIBC_O_WRONLY
#define O_RDWR ULIBC_O_RDWR
#define O_ACCMODE 3
#define O_CREAT ULIBC_O_CREAT
#define O_TRUNC ULIBC_O_TRUNC
#define O_APPEND ULIBC_O_APPEND
#define O_DIRECTORY ULIBC_O_DIRECTORY

#endif /* MICRO_OS_LIBC_USER_FCNTL_H */
