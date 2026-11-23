/*
 * Micro OS — the numbers of a device, for a portable source.
 *
 * The VFS of this system has no device numbers of its own: a node of /dev is
 * a name and a driver, not a major and a minor. The macros exist because a
 * listing of a file prints `major, minor` for a special file, and that path
 * is not reached when no object is a device.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_SYSMACROS_H
#define MICRO_OS_LIBC_USER_SYS_SYSMACROS_H

#define major(dev) ((unsigned)(((dev) >> 8) & 0xff))
#define minor(dev) ((unsigned)((dev) & 0xff))
#define makedev(maj, min) ((((unsigned long)(maj) & 0xff) << 8) | ((unsigned long)(min) & 0xff))

#endif /* MICRO_OS_LIBC_USER_SYS_SYSMACROS_H */
