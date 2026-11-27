/*
 * Micro OS — the control commands of a device, `ioctl` of POSIX.
 *
 * The ABI has had an `ioctl` since 1.0 (number 12), and until 1.9 it answered
 * `EINVAL` for every request: there was no device with a command of its own
 * and no terminal to have a mode (`docs/debts.md`). The framebuffer asked for
 * its two commands in 1.6 (ADR 0028) and the terminal of a program asks for
 * the five below in 1.9 (ADR 0051), so the header has names now: the numbers
 * are the ones of Linux, and a request is data of the call and not a system
 * call of its own.
 *
 * The request is an `unsigned long` and the third argument is a pointer of
 * the caller. The call answers zero, or -1 and `errno`.
 */
#ifndef MICRO_OS_LIBC_USER_SYS_IOCTL_H
#define MICRO_OS_LIBC_USER_SYS_IOCTL_H

#include "ulibc.h"

/* The size of a terminal in cells, the `struct winsize` of POSIX. */
struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* --- the terminal (ADR 0051 §3) --- */
#define TCGETS 0x5401u     /* struct termios *: read the mode */
#define TCSETS 0x5402u     /* const struct termios *: set it */
#define TCSETSW 0x5403u    /* the same, the output drained first */
#define TCSETSF 0x5404u    /* the same, the input dropped */
#define TIOCGPGRP 0x540Fu  /* pid_t *: foreground process group */
#define TIOCSPGRP 0x5410u  /* const pid_t *: set foreground process group */
#define TIOCGWINSZ 0x5413u /* struct winsize *: the size of the screen */
#define TIOCSWINSZ 0x5414u /* const struct winsize *: accepted, ignored */
#define FIONREAD 0x541Bu   /* int *: the bytes that wait to be read */

/* --- the framebuffer (ADR 0028) --- */
#define FBIO_TAKE 0x4D01u    /* the console of the machine is the program's */
#define FBIO_GIVE 0x4D02u    /* and it is given back */
#define FBIO_PRESENT 0x4D03u /* struct fb_present_surface *: flip or copy */

int ioctl(int fd, unsigned long request, ...);

#endif /* MICRO_OS_LIBC_USER_SYS_IOCTL_H */
