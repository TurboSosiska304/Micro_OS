/*
 * Micro OS — hello, the first program of the system that is a file.
 *
 * It is built by the port (ports/port_x86_64/CMakeLists.txt): the object of
 * this file is linked at the address of the window of the programs, turned
 * into an image of the MOX1 format by tools/mkprog.sh and embedded in the
 * kernel. At the boot the image is written to /bin/hello of the RAM volume,
 * read back through the VFS and run, so the bytes that are executed have been
 * through a file and not through a variable of the kernel.
 *
 * Nothing of the kernel is linked here and there is no libc: a program talks
 * to the system through the vector 0x80 and to nothing else (programs/prog.h,
 * ADR 0006). Every line below is a call - the console, the arguments, the
 * number of the task, the clock, a sleep - so the transcript of the machine
 * shows that the way back into the kernel works and that a result comes back:
 * a program that could only write would prove the door, not the answer.
 *
 * The program returns, and the port takes it back with the value of the
 * return in RAX as its status. A program of the system ends in a halt or in
 * the exit call of a process, neither of which exists yet; a demonstration
 * that comes back is also what lets the shell of the smoke test start at all.
 */
#include "prog.h"

#define PROGRAM_LINE "PROGRAM: hello from /bin\n"

/*
 * The entry of the format: the port enters a program with the count and the
 * vector of the arguments in the registers of the usual ABI (see
 * k_port_exec_enter), which is what the two parameters of a C program mean.
 *
 * The calls are checked: a failure is printed with its number, because a
 * program that says nothing when the kernel refused it is a program that
 * lies about the machine. The entry returns an int, and the value of that
 * return is the status the port reports to the kernel (they share the
 * convention of a C function of this machine): 0 is "every call answered",
 * 1 is "one of them did not", and the transcript says which one.
 */
int hello_main(int argc, char *const argv[])
{
    long wrote;

    wrote = prog_puts(PROGRAM_LINE);
    if (wrote < 0) {
        prog_puts("PROGRAM: write failed with ");
        prog_put_dec((unsigned long)(-wrote));
        prog_puts("\n");
        return 1;
    }

    /*
     * The vector the kernel built for this program. The count and the words
     * themselves are the proof that the arguments of a line of the shell
     * survive the call: `exec /bin/hello one two` is three words, and the
     * name of the program is the first of them, as in Linux.
     */
    prog_puts("PROGRAM: argc ");
    prog_put_dec((unsigned long)argc);
    if (argc > 0 && argv != NULL && argv[0] != NULL) {
        prog_puts(", argv[0] ");
        prog_puts(argv[0]);
    }
    prog_puts("\n");

    prog_puts("PROGRAM: pid ");
    prog_put_dec((unsigned long)prog_getpid());
    prog_puts(", up ");
    prog_put_dec((unsigned long)prog_uptime());
    prog_puts(" ms\n");

    /*
     * A call that waits is the one that proves the interrupts of the stub:
     * the sleep moves the kernel's clock forward and comes back here, and the
     * line after it is written by a program that was not running for a
     * moment. Ten milliseconds are short enough for the smoke test and long
     * enough to be a real tick of the timer.
     */
    if (prog_sleep(10) < 0) {
        prog_puts("PROGRAM: sleep failed\n");
        return 1;
    }

    prog_puts("PROGRAM: slept 10 ms, up ");
    prog_put_dec((unsigned long)prog_uptime());
    prog_puts(" ms\n");

    return 0;
}
