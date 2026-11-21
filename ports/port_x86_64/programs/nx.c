/*
 * Micro OS — nx, the program that proves a page is not writable and
 * executable at once.
 *
 * A program of ring 3 has a text of its own and a data of its own, and the
 * port gives them the two protections a page can carry (ADR 0007, NX and
 * W^X): the text is readable and executable and never writable, the data,
 * the bss and the stack are writable and never executable. This program asks
 * the processor to break the rule three times, in a child of its own each
 * time:
 *
 *   - the first child jumps into its data segment, and the fetch meets a page
 *     whose bit 63 says "no code here" - a page fault whose error code has
 *     the instruction-fetch bit (0x10) set;
 *   - the second child writes one byte of a function of its text, and the
 *     store meets a page without the write bit - a page fault, and this time
 *     the error code says "write, present, ring 3" (0x7).
 *
 * A third child asks the kernel to write into the same text page: a read
 * into a buffer that is code. The edge of the call copies into a read-only
 * page, and the port gives the space a private copy of the page as data
 * before the store (copy on write, for the other direction). The call
 * answers, the child's next instruction is fetched from a page that is no
 * longer code, and the child goes down with the instruction-fetch fault; the
 * kernel is alive and the text of the parent, which shares the original
 * frame, was not touched.
 *
 * The three children are taken down by the handler of the port with the
 * status 255; the parent waits for each of them and is still running, and it
 * ends with 0, so the shell that asked for the program answers the next
 * command. A machine without the protections would execute the data and
 * print a line from it, or take the write and go on: those lines are what
 * the smoke test looks for and must not find.
 *
 * The children are forks on purpose. The fault of a program is the end of the
 * task that runs it, so a single program can meet one fault and no more; the
 * parent is what makes the next fault possible after the first, and the
 * status of a child is the number the port chose for a fault.
 */
#include "prog.h"

/*
 * In the data of the program: initialised, writable and never executable. The
 * eight numbers are there so that the array is not folded into the text by a
 * compiler that knows it is never read; `volatile` keeps the store of the
 * second function and the address it takes.
 */
static volatile unsigned long g_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};

/* In the text of the program: executed and never writable. `used` keeps the
 * function in the image when its address is taken and the compiler would
 * otherwise fold the reference for a function nothing calls. */
__attribute__((noinline, used)) static void nx_text(void)
{
    g_data[0] = g_data[0] + 1u;
}

static void report(const char *what, long value)
{
    prog_puts("PROGRAM: ");
    prog_puts(what);
    prog_puts(" ");
    prog_put_dec((unsigned long)(value < 0 ? -value : value));
    prog_puts("\n");
}

/* One fault of a child: fork, run `body` in the child, wait and report. */
static long fault_in_child(const char *line, void (*body)(void))
{
    int status = -1;
    long pid = prog_fork();

    if (pid == 0) {
        prog_puts("PROGRAM: ");
        prog_puts(line);
        prog_puts("\n");
        body();
        prog_puts("PROGRAM: the fault did not happen\n");
        prog_exit(1);
        return 1;
    }

    if (pid < 0) {
        report("fork failed", -pid);
        return pid;
    }

    if (prog_waitpid(pid, &status, 0) < 0) {
        prog_puts("PROGRAM: waitpid failed\n");
        return -1;
    }

    return status;
}

static void execute_data(void)
{
    void (*entry)(void) = (void (*)(void))(unsigned long)g_data;

    entry();
}

static void write_text(void)
{
    unsigned long address = (unsigned long)(void (*)(void))nx_text;
    volatile unsigned char *code = (volatile unsigned char *)address;

    *code = 0x90u;
}

/*
 * The kernel writes into the text on behalf of the program. The page is not
 * writable, so the store of the kernel faults in ring 0; the port copies the
 * page, makes the copy data (writable and NX) and runs the store again. The
 * call returns, and the next fetch from that page is the fault of a program.
 */
static void kernel_writes_text(void)
{
    unsigned long address = (unsigned long)(void (*)(void))nx_text;
    int fd = (int)prog_open("/bin/hello", KSYS_O_RDONLY);

    if (fd < 0) {
        return;
    }

    (void)prog_read(fd, (void *)address, 1);
    (void)prog_close(fd);

    prog_puts("PROGRAM: the kernel wrote the text page\n");
}

int nx_main(int argc, char *const argv[])
{
    long status;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: nx starts\n");

    /* 1. The data of the program is not code. */
    status = fault_in_child("the child executes the data page", execute_data);
    if (status < 0) {
        prog_exit(1);
        return 1;
    }

    report("execute data, status", status);

    if (status != 255) {
        prog_puts("PROGRAM: the data page was executable\n");
        prog_exit(1);
        return 1;
    }

    /* 2. The text of the program is not memory. */
    status = fault_in_child("the child writes the text page", write_text);
    if (status < 0) {
        prog_exit(1);
        return 1;
    }

    report("write text, status", status);

    if (status != 255) {
        prog_puts("PROGRAM: the text page took a write\n");
        prog_exit(1);
        return 1;
    }

    /* 3. The same page, written by the kernel at the edge of a call. */
    status = fault_in_child("the child reads into the text page", kernel_writes_text);
    if (status < 0) {
        prog_exit(1);
        return 1;
    }

    report("kernel write text, status", status);

    if (status != 255) {
        prog_puts("PROGRAM: the kernel write left the program alive\n");
        prog_exit(1);
        return 1;
    }

    prog_puts("PROGRAM: the three faults left the machine alive\n");

    prog_exit(0);
    return 0;
}
