/*
 * Micro OS — the mode of a terminal, the calls of `<termios.h>`.
 *
 * The ABI has one call for all of it: `ioctl` (number 12) with `TCGETS` or
 * `TCSETS` and the pointer of the structure, which is the structure of the
 * kernel byte for byte (kernel/tty.h). The library is therefore a translation
 * of the shape of POSIX into that one call and nothing else: no state of its
 * own, no second copy of the mode.
 *
 * The terminal of a descriptor is the console of the task while nothing is
 * opened on 0, 1 or 2 (ADR 0051 §2), which is why a program that was written
 * for a Unix finds a terminal in the descriptors it inherited without opening
 * `/dev/tty` at all.
 */
#include <termios.h>

#include <errno.h>
#include <stdarg.h>

#include "ulibc.h"

#include "ulibc_internal.h"

/*
 * `ioctl` of POSIX: the descriptor, the request and the pointer of the caller.
 * The third argument is declared variadic because that is what the header of a
 * Unix says and a source of somebody else may pass an integer; only the
 * pointer form reaches the kernel.
 */
int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *argument;
    long rc;

    va_start(ap, request);
    argument = va_arg(ap, void *);
    va_end(ap);

    rc = __ulibc_result(
        __ulibc_syscall6(KSYS_IOCTL, fd, (long)request, (long)(uintptr_t)argument, 0, 0, 0));
    if (rc < 0) {
        return -1;
    }

    return (int)rc;
}

/* The mode of the terminal behind a descriptor: ENOTTY when the descriptor is
 * not a terminal, which is the answer a portable source tests. */
int tcgetattr(int fd, struct termios *settings)
{
    if (settings == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (ioctl(fd, TCGETS, settings) != 0) {
        /* The kernel answers EINVAL for a descriptor that is a file and has
         * no mode; a source of POSIX expects ENOTTY and branches on it. */
        if (errno == EINVAL) {
            errno = ENOTTY;
        }
        return -1;
    }

    return 0;
}

int tcsetattr(int fd, int action, const struct termios *settings)
{
    unsigned long request;

    if (settings == NULL) {
        errno = EINVAL;
        return -1;
    }

    switch (action) {
    case TCSANOW:
        request = TCSETS;
        break;
    case TCSADRAIN:
        request = TCSETSW;
        break;
    case TCSAFLUSH:
        request = TCSETSF;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    if (ioctl(fd, request, (void *)(uintptr_t)settings) != 0) {
        if (errno == EINVAL) {
            errno = ENOTTY;
        }
        return -1;
    }

    return 0;
}

void cfmakeraw(struct termios *settings)
{
    if (settings == NULL) {
        return;
    }

    /* The recipe of `cfmakeraw`: no translation of the input, no break, no
     * parity, no flow control, no post-processing of the output, eight bits,
     * no canonical mode, no echo, no signals, no extensions, and a read that
     * returns each byte as it comes. The structure is the one of the kernel,
     * so the mask is the same bits and the two agree by construction. */
    settings->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    settings->c_oflag &= ~OPOST;
    settings->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    settings->c_cflag &= ~(CSIZE | PARENB);
    settings->c_cflag |= CS8;
    settings->c_cc[VMIN] = 1;
    settings->c_cc[VTIME] = 0;
}

/*
 * The speed of the line. This system has one, so the getters answer what the
 * structure carries (the speed is not a field of the `struct termios` of
 * Linux either: the two callers keep it in `c_cflag` with `B*` bits, and a
 * terminal of this system carries them and does not read them).
 */
speed_t cfgetispeed(const struct termios *settings)
{
    if (settings == NULL) {
        return B0;
    }

    return settings->c_cflag & 0xF;
}

speed_t cfgetospeed(const struct termios *settings)
{
    return cfgetispeed(settings);
}

int cfsetispeed(struct termios *settings, speed_t speed)
{
    if (settings == NULL || speed > 0xFu) {
        /* A number of a speed this machine does not have is not a mistake of
         * the program that can be repaired here: the line is fixed. */
        return 0;
    }

    settings->c_cflag = (settings->c_cflag & ~0xFu) | speed;

    return 0;
}

int cfsetospeed(struct termios *settings, speed_t speed)
{
    return cfsetispeed(settings, speed);
}

/*
 * Drop what waits for a terminal. There is no queue of the library and none
 * of the console: the bytes of the input that were already typed are the line
 * of the discipline, and `TCSETSF` is the call that drops them. So the answer
 * here is the question of the terminal itself - a descriptor that is not one
 * is ENOTTY, and one that is a terminal has nothing to drop - and zero.
 */
int tcflush(int fd, int queue)
{
    struct termios settings;

    if (queue != TCIFLUSH && queue != TCOFLUSH && queue != TCIOFLUSH) {
        errno = EINVAL;
        return -1;
    }

    if (tcgetattr(fd, &settings) != 0) {
        return -1;
    }

    /* The input is dropped by setting the mode the terminal already has, with
     * TCSAFLUSH: the one call that carries "and forget the line". */
    if (queue == TCIFLUSH || queue == TCIOFLUSH) {
        return tcsetattr(fd, TCSAFLUSH, &settings);
    }

    return 0;
}

int tcdrain(int fd)
{
    struct termios settings;

    if (tcgetattr(fd, &settings) != 0) {
        return -1;
    }

    /* The output of this system is written when it is written: there is no
     * queue of the terminal to drain. */
    return 0;
}
