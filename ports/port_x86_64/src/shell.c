/*
 * Micro OS — the shell of the x86-64 port.
 *
 * The interpreter belongs to the kernel (components/kshell), so the commands
 * are the same as on the board and the port adds none of its own. What this
 * file supplies is the console under the interpreter, and that is the whole
 * reason the file exists: COM1 is polled, a read answers "nothing yet"
 * instead of waiting, and the shell of the kernel reads until the end of the
 * input. Handing it a polled console would make it treat the first moment
 * without a key as the end of the stream and execute a half typed line as a
 * command, so the port waits for the characters itself and calls
 * kshell_execute() once a whole line is there.
 */
#include "kshell/kshell.h"

#include "kernel/console.h"
#include "kernel/sched.h"
#include "kernel/types.h"
#include "kernel/version.h"

/*
 * The nap of the shell while no key is waiting. It is short, because a
 * person notices a delay of a tenth of a second, and long enough that the
 * idle loop costs nothing worth counting.
 */
#define SHELL_IDLE_MS 20u

/*
 * Read one line from the console, echoing it, with the editing the shell of
 * the board has: backspace erases a character, Ctrl+C throws the line away,
 * and a line that did not fit is not executed, because a cut line is a
 * different command.
 *
 * Returns the length of the line, or -1 when there is nothing to execute
 * (the line was thrown away). There is no end of the input on a serial
 * port: the loop never ends by itself.
 */
static int shell_read_line(char *buf, k_size_t size)
{
    k_size_t len = 0;
    bool cut = false;

    for (;;) {
        int c = k_console_poll();

        if (c < 0) {
            /* COM1 has nothing yet: let the other tasks run meanwhile. */
            k_sched_sleep_ms(SHELL_IDLE_MS);
            continue;
        }

        if (c == '\r' || c == '\n') {
            k_console_write("\r\n", 2);
            buf[len] = '\0';

            if (cut) {
                kshell_printf("line too long, it was not executed\n");
                return -1;
            }

            return (int)len;
        }

        if (c == '\b' || c == 127) {
            if (len > 0) {
                len--;
                k_console_write("\b \b", 3);
            }
            continue;
        }

        if (c == 3) { /* Ctrl+C: the line is thrown away */
            k_console_write("^C\r\n", 4);
            len = 0;
            cut = false;
            continue;
        }

        if (len + 1 < size) {
            buf[len++] = (char)c;
            k_console_write(&buf[len - 1], 1);
        } else {
            cut = true;
        }
    }
}

/*
 * The body of the shell task. kshell_init() has run before the task was
 * created, so the commands are registered and the banner is the first thing
 * a person sees on the port.
 */
void port_x86_64_shell_run(void)
{
    char line[KSHELL_LINE_MAX];
    int rc;

    kshell_printf("%s shell ready. Type 'help' for the list of commands.\n", kernel_name());

    for (;;) {
        kshell_printf("microos> ");

        rc = shell_read_line(line, sizeof(line));
        if (rc < 0) {
            continue;
        }

        kshell_execute(line);
    }
}
