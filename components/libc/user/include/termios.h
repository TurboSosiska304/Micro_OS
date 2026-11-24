/*
 * Micro OS — the mode of a terminal, the `termios` of POSIX.
 *
 * A program of this system found no terminal behind its descriptors until
 * 1.9 (ADR 0051): `tcgetattr` answered `ENOTTY` and the header had no flags to
 * set. The console of the machine is a terminal now, and this header is its
 * mode: the flags, the control characters, and the five calls a portable
 * source uses - `tcgetattr`, `tcsetattr`, `cfmakeraw` and the two pairs of the
 * speed.
 *
 * The numbers are the ones of Linux, and `struct termios` is the structure of
 * the ABI byte for byte (kernel/tty.h): the library hands the pointer of the
 * caller to the kernel and nothing is translated, which is what makes a mode
 * that a program of another compiler set the mode this system reads.
 *
 * What this system implements is written in the comment of every flag. The
 * speeds are no-ops: the UART of the board and the line of x86-64 are
 * programmed once at the boot, and a program that asks for another number is
 * told zero and not a lie.
 */
#ifndef MICRO_OS_LIBC_USER_TERMIOS_H
#define MICRO_OS_LIBC_USER_TERMIOS_H

#include "ulibc.h"

/* The requests of the mode are the ones of `<sys/ioctl.h>`, where Linux keeps
 * them as well: `TCGETS` is asked with `ioctl` and there is no second copy of
 * the numbers. */
#include <sys/ioctl.h>

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

/* The control characters of the line, the Linux number: the five this system
 * reads are named below the structure. */
#define NCCS 19

struct termios {
    tcflag_t c_iflag; /* the modes of the input */
    tcflag_t c_oflag; /* the modes of the output */
    tcflag_t c_cflag; /* the mode of the line: eight bits, no parity */
    tcflag_t c_lflag; /* the modes of the line discipline */
    cc_t c_line;
    cc_t c_cc[NCCS];
};

/* --- the input modes --- */
#define IGNBRK 0000001u  /* a break is ignored */
#define BRKINT 0000002u  /* a break is an interrupt: carried, not implemented */
#define IGNPAR 0000004u  /* a byte with a parity error is dropped */
#define PARMRK 0000010u  /* the marking of a parity error: carried */
#define INPCK 0000020u   /* the parity of the input is checked: carried */
#define ISTRIP 0000040u  /* the eighth bit is stripped: carried */
#define INLCR 0000100u   /* a NL is a CR: not implemented */
#define IGNCR 0000200u   /* a CR is dropped */
#define ICRNL 0000400u   /* a CR is a NL, which is what the keyboard sends */
#define IXON 0002000u    /* the flow control of the output: carried, not used */
#define IXANY 0004000u   /* any character restarts: carried */
#define IXOFF 0010000u   /* the flow control of the input: carried */
#define IMAXBEL 0020000u /* the bell of a full input: carried */
#define IUTF8 0040000u   /* the input is UTF-8: carried */

/* --- the output modes --- */
#define OPOST 0000001u  /* the output is post-processed, which the console does */
#define ONLCR 0000004u  /* a NL is a CR NL: the console breaks the line itself */
#define OCRNL 0000010u  /* a CR is a NL: not implemented */
#define ONOCR 0000020u  /* no CR in the first column: not implemented */
#define ONLRET 0000040u /* a NL returns the carriage: not implemented */

/* --- the modes of the line --- */
#define CSIZE 0000060u /* the mask of the bits of a character */
#define CS5 0000000u
#define CS6 0000020u
#define CS7 0000040u
#define CS8 0000060u    /* eight bits: the one this system has */
#define CSTOPB 0000100u /* two stop bits: carried */
#define CREAD 0000200u  /* the receiver is enabled */
#define PARENB 0000400u /* a parity bit: carried, not generated */
#define PARODD 0001000u /* odd parity: carried */
#define CLOCAL 0004000u /* the line ignores the modem control: carried */

/* --- the modes of the discipline --- */
#define ISIG 0000001u    /* the characters of a signal are signals */
#define ICANON 0000002u  /* a line at a time: erase, kill and the end of a file */
#define ECHO 0000010u    /* the tty repeats what the person types */
#define ECHOE 0000020u   /* the erase is "\b \b" and not "^H" */
#define ECHOK 0000040u   /* the kill is echoed to the start of the line */
#define ECHONL 0000100u  /* a NL is echoed even with ECHO off */
#define NOFLSH 0000200u  /* the input is not dropped by a signal: carried */
#define TOSTOP 0000400u  /* a write of the background stops: carried */
#define ECHOCTL 0001000u /* a control character is echoed as "^X": carried */
#define ECHOKE 0004000u  /* the erase of a kill is done well: carried */
#define IEXTEN 0100000u  /* the extensions of the line: carried */

/* --- the indices of c_cc --- */
#define VINTR 0u  /* the interrupt: ^C */
#define VQUIT 1u  /* the quit: ^\ */
#define VERASE 2u /* the erase of a character: DEL */
#define VKILL 3u  /* the erase of the line: ^U */
#define VEOF 4u   /* the end of the file: ^D */
#define VTIME 5u  /* the tenths of a second of a raw read */
#define VMIN 6u   /* the bytes of a raw read */
#define VSWTC 7u
#define VSTART 8u /* the start of the output: ^Q */
#define VSTOP 9u  /* the stop of the output: ^S */
#define VSUSP 10u /* the suspend: ^Z */
#define VEOL 11u  /* the end of a line of its own */
#define VREPRINT 12u
#define VDISCARD 13u
#define VWERASE 14u
#define VLNEXT 15u
#define VEOL2 16u

/* --- the speeds, which are no-ops of this system --- */
#define B0 0000000u
#define B50 0000001u
#define B75 0000002u
#define B110 0000003u
#define B134 0000004u
#define B150 0000005u
#define B200 0000006u
#define B300 0000007u
#define B600 0000008u
#define B1200 0000009u
#define B1800 0000010u
#define B2400 0000011u
#define B4800 0000012u
#define B9600 0000013u
#define B19200 0000014u
#define B38400 0000015u
#define B57600 0010001u
#define B115200 0010002u
#define B230400 0010003u

/* --- the actions of tcsetattr --- */
#define TCSANOW 0   /* set the mode now */
#define TCSADRAIN 1 /* drain the output first: the same here */
#define TCSAFLUSH 2 /* and drop the input that was not read */

/* The mode of the terminal behind a descriptor: zero, or -1 and `errno`. */
int tcgetattr(int fd, struct termios *settings);
int tcsetattr(int fd, int action, const struct termios *settings);

/*
 * The raw mode of a screen: no canonical mode, no echo, no signals, no
 * translation of the input, no post-processing of the output, eight bits, and
 * a read that returns each byte as it comes (VMIN 1, VTIME 0).
 */
void cfmakeraw(struct termios *settings);

/* The speed of a line. There is one, and these calls say so: the getters
 * answer what the structure carries, the setters keep it and answer zero. */
speed_t cfgetispeed(const struct termios *settings);
speed_t cfgetospeed(const struct termios *settings);
int cfsetispeed(struct termios *settings, speed_t speed);
int cfsetospeed(struct termios *settings, speed_t speed);

/* Drop the input (TCIFLUSH), the output (TCOFLUSH) or both that wait for the
 * descriptor. The terminal of this system has no queue of its own, so the
 * call is the setting of the mode it already has: zero, or -1 and `errno`. */
#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2
int tcflush(int fd, int queue);
int tcdrain(int fd);

#endif /* MICRO_OS_LIBC_USER_TERMIOS_H */
