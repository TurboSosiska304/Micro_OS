/*
 * Micro OS — poll, the wait for a descriptor.
 *
 * The program is the gate of the first lane of 1.6: it asks the kernel about
 * the state of two pipes at once, waits for a byte that a child writes while
 * it waits, waits for a deadline that nothing ends, and sleeps on a list of
 * no descriptor. Every answer is a line of the console, so the smoke test of
 * the machine reads what the call did and not only that it returned.
 *
 * A wait of a pipe is the scheduler's: while this program waits for the byte
 * of its child it is out of the run queue, and the child is what runs. The
 * line with the number of the milliseconds the deadline took is the proof
 * that the wait was a wait and not a spin of the processor - a spin would
 * answer at once or never.
 */
#include "ulibc.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#define POLL_LINE 128

static void say(const char *text)
{
    (void)write(1, text, strlen(text));
}

static void say_number(const char *label, long value)
{
    char line[POLL_LINE];
    int length = snprintf(line, sizeof(line), "%s%ld\n", label, value);

    if (length > 0) {
        (void)write(1, line, (unsigned long)length);
    }
}

int main(int argc, char *const argv[], char *const envp[])
{
    struct pollfd list[2];
    char byte = 0;
    int a[2];
    int b[2];
    int c[2];
    int pid;
    int status = 0;
    long started;
    long elapsed;

    (void)argc;
    (void)argv;
    (void)envp;

    /*
     * The console of the program. A descriptor of this system is the console
     * when it is not open as a file, and a pipe takes the lowest free slot:
     * without this the first pipe would be 0 and 1 and the lines of the
     * program would go into it. The device is put on 0, 1 and 2, which is
     * what a program of a shell expects.
     */
    {
        int console = open("/dev/tty0", O_WRONLY);

        if (console >= 0) {
            (void)dup2(console, 1);

            if (console > 2) {
                (void)close(console);
            }
        }
    }

    if (pipe(a) != 0 || pipe(b) != 0 || pipe(c) != 0) {
        say("poll: no pipes\n");
        return 1;
    }

    /* Two empty pipes: nothing is ready, and a timeout of zero does not
     * wait for it. */
    list[0].fd = a[0];
    list[0].events = POLLIN;
    list[0].revents = 0;
    list[1].fd = b[0];
    list[1].events = POLLIN;
    list[1].revents = 0;

    say_number("poll: empty, ready=", poll(list, 2, 0));

    /* A byte in the second pipe is the answer of the second entry only. */
    if (write(b[1], "y", 1) != 1) {
        say("poll: no byte into the pipe\n");
        return 1;
    }

    say_number("poll: one byte, ready=", poll(list, 2, 0));
    say_number("poll: the second entry says ", list[1].revents);

    if (read(b[0], &byte, 1) != 1 || byte != 'y') {
        say("poll: the byte did not come back\n");
        return 1;
    }

    /* A deadline that nothing ends: the call answers zero after the time it
     * was given, and the time is the one of the clock of the machine. */
    list[0].fd = a[0];
    list[0].events = POLLIN;
    started = (long)uptime();
    say_number("poll: timeout, ready=", poll(list, 1, 120));
    elapsed = (long)uptime() - started;
    say_number("poll: timeout, ms=", elapsed);

    /* A list of no descriptor is a sleep: the scheduler keeps the processor
     * for the other tasks and the call answers when the time is over. */
    started = (long)uptime();
    say_number("poll: sleep, ready=", poll(NULL, 0, 60));
    elapsed = (long)uptime() - started;
    say_number("poll: sleep, ms=", elapsed);

    /* The wait that the object ends: the child writes a byte after a moment,
     * and the poll of the reader - without a deadline - answers with it. */
    pid = fork();

    if (pid == 0) {
        (void)usleep(80000);
        (void)write(a[1], "z", 1);
        exit(0);
    }

    if (pid < 0) {
        say("poll: no child\n");
        return 1;
    }

    list[0].fd = a[0];
    list[0].events = POLLIN;
    started = (long)uptime();
    say_number("poll: wait, ready=", poll(list, 1, -1));
    elapsed = (long)uptime() - started;
    say_number("poll: wait, ms=", elapsed);

    if (read(a[0], &byte, 1) == 1) {
        char line[8];

        line[0] = 'p';
        line[1] = 'o';
        line[2] = 'l';
        line[3] = 'l';
        line[4] = ':';
        line[5] = ' ';
        line[6] = byte;
        line[7] = '\n';
        (void)write(1, line, sizeof(line));
    }

    (void)waitpid(pid, &status, 0);

    /* The end of the writing end is the end of the file: a read does not
     * wait, so the entry is ready. */
    (void)close(a[1]);
    list[0].fd = a[0];
    list[0].events = POLLIN;
    say_number("poll: end of file, ready=", poll(list, 1, 0));
    say_number("poll: end of file says ", list[0].revents);

    /* A writing end whose reader is gone is the error of the object. */
    (void)close(c[0]);
    list[0].fd = c[1];
    list[0].events = POLLOUT;
    say_number("poll: broken pipe, ready=", poll(list, 1, 0));
    say_number("poll: broken pipe says ", list[0].revents);

    /* A number that is not an open descriptor is the error of the entry. */
    list[0].fd = 15;
    list[0].events = POLLIN;
    say_number("poll: bad descriptor says ", poll(list, 1, 0) == 1 ? list[0].revents : -1);

    say("poll: done\n");

    return 0;
}
