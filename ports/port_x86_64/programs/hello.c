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
 * ADRs 0006 and 0007). Every line below is a call - the console, the
 * arguments, the number of the task, the clock, a sleep - so the transcript
 * of the machine shows that the way back into the kernel works and that a
 * result comes back: a program that could only write would prove the door,
 * not the answer.
 *
 * The program is ring 3 code with page tables of its own. The line that says
 * the current privilege level reads CS and prints its low two bits: it is the
 * one number of this transcript that the kernel could not write for it, and
 * it is 3 when the program is a user and not a function of the kernel.
 *
 * The program ends with the exit call of a process (KSYS_EXIT), not with a
 * return: a return in ring 3 has no kernel frame to land in, and the port
 * takes the program down through the call so that the shell that ran it is
 * still alive afterwards.
 */
#include "prog.h"

#define PROGRAM_LINE "PROGRAM: hello from /bin\n"

/* The low two bits of CS: 0 for the kernel, 3 for a program of ring 3. */
static unsigned long current_cpl(void)
{
    unsigned short cs;

    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return (unsigned long)(cs & 3u);
}

/*
 * The entry of the format: the port enters a program with the count, the
 * vector of the arguments and the vector of the environment in the registers
 * of the usual ABI (see k_port_exec_enter and ADR 0016), which is what the
 * three parameters of a C program mean.
 *
 * The calls are checked: a failure is printed with its number, because a
 * program that says nothing when the kernel refused it is a program that
 * lies about the machine. A return of 1 says "one of them did not answer".
 */
int hello_main(int argc, char *const argv[], char *const envp[])
{
    long wrote;
    int envs = 0;

    wrote = prog_puts(PROGRAM_LINE);
    if (wrote < 0) {
        prog_puts("PROGRAM: write failed with ");
        prog_put_dec((unsigned long)(-wrote));
        prog_puts("\n");
        return 1;
    }

    /*
     * The privilege level of the moment. It is read by the program and not
     * handed to it: a kernel could print "cpl 3" without ever leaving ring 0,
     * and the smoke test would have no way to tell.
     */
    prog_puts("PROGRAM: cpl ");
    prog_put_dec(current_cpl());
    prog_puts("\n");

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

    /*
     * The environment of the program, which the kernel filled in when the
     * call carried none (k_exec_environ): the count and the first word. The
     * entry ABI is argc, argv and envp in RDI, RSI and RDX (ADR 0016), and a
     * program that prints the vector proves that the third register survived
     * the crossing into ring 3.
     */
    if (envp != NULL) {
        while (envp[envs] != NULL) {
            envs++;
        }
    }

    prog_puts("PROGRAM: envp ");
    prog_put_dec((unsigned long)envs);
    if (envs > 0) {
        prog_puts(", ");
        prog_puts(envp[0]);
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

    prog_exit(0);

    return 0;
}
