/*
 * Micro OS — the terminal that is not one.
 *
 * The console of the x86-64 port is a screen and a serial line, and the kernel
 * has no control command for either: `ioctl` answers EINVAL for every request,
 * and `isatty` of this library says no. The two calls are here because a
 * portable command includes `<termios.h>` and asks; the answer is the error of
 * a descriptor that is not a terminal, never a mode that the machine does not
 * have.
 */
#include <termios.h>

#include <errno.h>

int tcgetattr(int fd, struct termios *settings)
{
    (void)fd;
    (void)settings;

    errno = ENOTTY;
    return -1;
}

int tcsetattr(int fd, int action, const struct termios *settings)
{
    (void)fd;
    (void)action;
    (void)settings;

    errno = ENOTTY;
    return -1;
}
