/*
 * Micro OS — mmu_io, the window of a copy that sleeps.
 *
 * ADR 0052 opens a window (STAC/CLAC) around the copy the kernel makes
 * through a pointer of a program, and a read or a write of this system hands
 * the buffer of the program to the file layer: the window is the call. A call
 * can sleep inside that window - a pipe with no room, a pipe with no byte, a
 * terminal with no key - and the copy that follows the sleep is still the
 * operation the program asked for. The flag of the window is a flag of the
 * core, the switch does not carry it, and the port therefore keeps it in the
 * context: a task that comes back from a sleep in the middle of a copy has to
 * find its window open, and the next task must not find it open by accident.
 *
 * This program is the case of the suite that a read and a write without a
 * sleep cannot make:
 *
 *   - the parent reads two bytes from a pipe that its child fills after a
 *     tenth of a second, so the copy into the buffer of the program happens
 *     after the switch back from the sleep;
 *   - the parent writes 8 KiB into a pipe of 4 KiB whose reader is a child
 *     that sleeps first, so the second half of the buffer is copied after the
 *     same kind of switch, in the other direction.
 *
 * A machine without SMAP passes this test whatever the port does with the
 * window; the machine of `make mmu-smoke` is `-cpu qemu64,+smep,+smap`, and
 * there a window lost across the switch is a page fault of ring 0 on the
 * buffer of the program - the machine stops, and the suite says so.
 */
#include "prog.h"

#define MMU_IO_BYTES 8192
#define MMU_IO_CHUNK 512

/*
 * The buffer is in the bss and not on the stack: 8 KiB is not a frame, and
 * the point of the program is the copy and not the stack that grows.
 */
static unsigned char g_io[MMU_IO_BYTES];

static long read_fill(void)
{
    long pid;
    int fds[2];

    if (prog_pipe(fds) != 0) {
        return -1;
    }

    pid = prog_fork();
    if (pid == 0) {
        (void)prog_call(KSYS_CLOSE, fds[0], 0, 0, 0, 0, 0);
        (void)prog_sleep(100);
        (void)prog_write(fds[1], "ok", 2);
        (void)prog_call(KSYS_CLOSE, fds[1], 0, 0, 0, 0, 0);
        prog_exit(0);

        return 0;
    }

    (void)prog_call(KSYS_CLOSE, fds[1], 0, 0, 0, 0, 0);

    /* The sleep is here: the child has not written yet, so the copy into
     * g_io happens after the parent comes back from the wait. */
    if (prog_read(fds[0], g_io, 2) != 2 || g_io[0] != 'o' || g_io[1] != 'k') {
        return -1;
    }

    (void)prog_call(KSYS_CLOSE, fds[0], 0, 0, 0, 0, 0);
    (void)prog_waitpid(pid, NULL, 0);

    return 0;
}

static long write_blocks(int fd, unsigned long bytes)
{
    unsigned long at;

    for (at = 0; at < bytes; at += MMU_IO_CHUNK) {
        unsigned long chunk = bytes - at;

        if (chunk > MMU_IO_CHUNK) {
            chunk = MMU_IO_CHUNK;
        }

        if (prog_write(fd, &g_io[at], chunk) != (long)chunk) {
            return -1;
        }
    }

    return 0;
}

static long write_fill(void)
{
    long pid;
    int fds[2];
    unsigned long at;

    for (at = 0; at < MMU_IO_BYTES; at++) {
        g_io[at] = (unsigned char)(at & 0xFFu);
    }

    if (prog_pipe(fds) != 0) {
        return -1;
    }

    pid = prog_fork();
    if (pid == 0) {
        long got;
        long total = 0;

        (void)prog_call(KSYS_CLOSE, fds[1], 0, 0, 0, 0, 0);
        (void)prog_sleep(100);

        while ((got = prog_read(fds[0], g_io, MMU_IO_CHUNK)) > 0) {
            total += got;
        }

        (void)prog_call(KSYS_CLOSE, fds[0], 0, 0, 0, 0, 0);
        prog_exit(total == (long)MMU_IO_BYTES ? 0 : 1);

        return 0;
    }

    (void)prog_call(KSYS_CLOSE, fds[0], 0, 0, 0, 0, 0);

    /* The pipe holds 4 KiB: the second half of this write sleeps in the
     * window until the child reads. */
    if (write_blocks(fds[1], MMU_IO_BYTES) != 0) {
        return -1;
    }

    (void)prog_call(KSYS_CLOSE, fds[1], 0, 0, 0, 0, 0);

    {
        int status = -1;

        if (prog_waitpid(pid, &status, 0) < 0 || status != 0) {
            return -1;
        }
    }

    return 0;
}

int mmu_io_main(int argc, char *const argv[])
{
    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: mmu_io starts\n");

    if (read_fill() != 0) {
        prog_puts("PROGRAM: the read across the sleep did not come back\n");
        return 1;
    }

    prog_puts("PROGRAM: the read of the pipe came back after the sleep\n");

    if (write_fill() != 0) {
        prog_puts("PROGRAM: the write across the sleep did not come back\n");
        return 1;
    }

    prog_puts("PROGRAM: the write of the pipe came back after the sleep\n");
    prog_puts("PROGRAM: a copy that sleeps keeps its window\n");

    prog_exit(0);

    return 0;
}
