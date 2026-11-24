/*
 * Micro OS — `/bin/keys`, the keyboard as events and not as characters
 * (ADR 0028, ADR 0040).
 *
 * The shell reads characters from the console and a keyboard that types them
 * is not the whole of a keyboard: a release, a modifier and a key that has no
 * character are events of `/dev/input0`, and a machine whose keyboard is a
 * PS/2 and a machine whose keyboard is a device of the USB have to give a
 * program the same ones. This program is the reader the smoke test of the
 * machine needs: it takes the keyboard away from the console, prints the code
 * and the edge of every event it reads, and gives it back, so a run that
 * types a key through the USB reads the event of that key on the serial port
 * and not only the letter the shell echoed.
 *
 * The code and the value are the numbers of the contract (kernel/input.h),
 * printed as they are: the test compares them with the table, and a program
 * that needs the name of a key has `k_input_key_name()` in the kernel.
 */
#include "ulibc.h"

#include <fcntl.h>
#include <unistd.h>

#include "kernel/input.h"

/* The events this program waits for and the time it waits: a keyboard that
 * is held by a program that never ends is a shell that never gets its keys
 * back, so the wait has a limit of its own. */
#define KEYS_EVENTS 8
#define KEYS_TRIES 200
#define KEYS_NAP_MS 10

int main(int argc, char *const argv[], char *const envp[])
{
    int fd;
    int count = 0;
    int tries;

    (void)argc;
    (void)argv;
    (void)envp;

    fd = open("/dev/input0", O_RDONLY);
    if (fd < 0) {
        (void)write(1, "keys: no /dev/input0\n", 21);
        return 1;
    }

    for (tries = 0; tries < KEYS_TRIES && count < KEYS_EVENTS; tries++) {
        struct input_event event;
        int read_now = (int)read(fd, &event, sizeof(event));

        if (read_now == (int)sizeof(event)) {
            char line[40];
            int length = snprintf(line, sizeof(line), "keys: %u %u %u\n", (unsigned)event.ms,
                                  (unsigned)event.code, (unsigned)event.value);

            if (length > 0) {
                (void)write(1, line, (unsigned long)length);
            }
            count++;
        } else {
            usleep(KEYS_NAP_MS * 1000);
        }
    }

    (void)close(fd);

    {
        char line[48];
        int length = snprintf(line, sizeof(line), "keys: done with %d event(s)\n", count);

        if (length > 0) {
            (void)write(1, line, (unsigned long)length);
        }
    }

    return 0;
}
