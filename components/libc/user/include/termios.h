/*
 * Micro OS — a terminal that is not one.
 *
 * The console of the x86-64 port is a VGA screen and a serial line, and the
 * kernel has no control command for either: `ioctl` answers `EINVAL` for every
 * request. A program that asks whether a descriptor is a terminal gets 0, and
 * a program that tries to change the mode of one gets -1 with `ENOTTY`. The
 * header exists because a portable source includes it; nothing of it reaches a
 * driver of this system.
 */
#ifndef MICRO_OS_LIBC_USER_TERMIOS_H
#define MICRO_OS_LIBC_USER_TERMIOS_H

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

#define NCCS 19

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
};

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *settings);
int tcsetattr(int fd, int action, const struct termios *settings);

#endif /* MICRO_OS_LIBC_USER_TERMIOS_H */
