/*
 * Micro OS — the terminal of a program, asked the way a program asks it.
 *
 * This is the second half of the gate of ADR 0051 and it is ours: it is not a
 * port of anything, it is the four questions a full-screen program asks before
 * it draws - is this a terminal, what is its mode, what is the size of the
 * screen, and does the mode go back - printed in words a transcript can check.
 *
 * The program never opens `/dev/tty0` for the first three: the descriptors 0
 * and 1 are the terminal of the task while nothing is opened on them
 * (ADR 0051 §2), which is the rule that lets an editor of somebody else run
 * here without a line of its source being changed. The last line opens
 * `/dev/tty`, the name POSIX gives the same device, and asks again.
 */
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* The four flags of a mode, as the words of the transcript. */
static void print_mode(const char *what, const struct termios *mode)
{
    printf("%s canonical=%d echo=%d isig=%d vmin=%u vtime=%u\n", what,
           (mode->c_lflag & ICANON) != 0, (mode->c_lflag & ECHO) != 0, (mode->c_lflag & ISIG) != 0,
           (unsigned)mode->c_cc[VMIN], (unsigned)mode->c_cc[VTIME]);
}

int main(void)
{
    struct termios mode;
    struct termios raw;
    struct termios back;
    struct winsize size;
    int fd;

    printf("tty: isatty(0)=%d isatty(1)=%d isatty(9)=%d\n", isatty(STDIN_FILENO),
           isatty(STDOUT_FILENO), isatty(9));

    if (tcgetattr(STDIN_FILENO, &mode) != 0) {
        printf("tty: tcgetattr of the console failed\n");
        return 1;
    }
    print_mode("tty: mode", &mode);

    memset(&size, 0, sizeof(size));
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0) {
        printf("tty: TIOCGWINSZ failed\n");
        return 1;
    }
    printf("tty: size %ux%u\n", (unsigned)size.ws_row, (unsigned)size.ws_col);

    raw = mode;
    cfmakeraw(&raw);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        printf("tty: tcsetattr of the raw mode failed\n");
        return 1;
    }
    if (tcgetattr(STDIN_FILENO, &back) != 0) {
        printf("tty: tcgetattr after cfmakeraw failed\n");
        return 1;
    }
    print_mode("tty: raw", &back);

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &mode) != 0) {
        printf("tty: tcsetattr back failed\n");
        return 1;
    }
    if (tcgetattr(STDIN_FILENO, &back) != 0) {
        printf("tty: tcgetattr back failed\n");
        return 1;
    }
    print_mode("tty: back", &back);

    fd = open("/dev/tty", O_RDONLY);
    printf("tty: /dev/tty isatty=%d\n", (fd >= 0) ? isatty(fd) : 0);
    if (fd >= 0) {
        (void)close(fd);
    }

    fd = open("/etc/setting", O_RDONLY);
    printf("tty: a file isatty=%d\n", (fd >= 0) ? isatty(fd) : 0);
    if (fd >= 0) {
        (void)close(fd);
    }

    return 0;
}
