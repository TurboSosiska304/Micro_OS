/*
 * Micro OS — the entry of a program of the x86-64 port.
 *
 * The port enters an image with the arguments of a C function in the
 * registers of the usual ABI (ADR 0006, ADR 0016): the count in RDI, the
 * vector of the arguments in RSI and the vector of the environment in RDX,
 * with the stack aligned as after a call. So `_start` is a C function with
 * three parameters and nothing of the assembly a start-up of a Unix-like
 * system needs: there is no dynamic linker to run, no constructors of shared
 * objects, and no libc to initialise.
 *
 * The program is entered, not called: a `ret` from here has no kernel frame
 * to land in, and the top word of the stack is a zero the port left as a
 * guard. main() therefore ends with exit(), which is a call of the kernel and
 * ends the task the program runs in. The loop after it is unreachable; it is
 * there so that a build that did not see the noreturn of exit still gets a
 * defined end instead of running down the stack.
 */
#include "ulibc.h"

/* The program itself, in the file of the program and not in the library. */
int main(int argc, char *const argv[], char *const envp[]);

void _start(long argc, char *const argv[], char *const envp[])
{
    exit(main((int)argc, argv, envp));

    for (;;) {
    }
}
