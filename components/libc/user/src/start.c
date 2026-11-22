/*
 * Micro OS — the entry of a program.
 *
 * On x86-64 the port enters an image with the arguments of a C function in
 * the registers of the usual ABI (ADR 0006, ADR 0016): the count in RDI, the
 * vector of the arguments in RSI and the vector of the environment in RDX,
 * with the stack aligned as after a call. So `_start` is a C function with
 * three parameters and nothing of the assembly a start-up of a Unix-like
 * system needs: there is no dynamic linker to run, no constructors of shared
 * objects, and no libc to initialise.
 *
 * The board (ADR 0018) adds one argument in front of the three: the address
 * of the door, the one function of the kernel a program may call. There is no
 * instruction a program of the ESP32 may execute to enter the kernel (the
 * vector of ESP-IDF takes `syscall` before the port sees it), so the port
 * hands the program a pointer to `ksys_call()` instead, and every wrapper of
 * the library goes through the copy kept here. The entry still has the shape
 * of a function of the windowed ABI, because the port calls it like one.
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

/*
 * The environment of the program, kept for `getenv` of the library. `_start`
 * is the only place that sees the third argument, and a source that asks for
 * a name (a `PATH` of a command, the `HOME` of a listing) reads it here. The
 * vector is not copied: the port left it in the window of the program, and it
 * lives as long as the program does.
 */
char *const *__ulibc_environ;

/* The name a portable program knows the vector by, for `environ`. */
char **environ;

static void start_set_environment(char *const envp[])
{
    __ulibc_environ = (char *const *)envp;
    environ = (char **)envp;
}

#ifdef MOX_ARCH_XTENSA
/*
 * The door of the program: the address of a function of the kernel with the
 * shape of `ksys_call(number, a0 ... a5)`. It lives in the data of the
 * program, so every wrapper of the library reads the same pointer _start was
 * given; a program that was not entered by the port has zero here and the
 * first call faults, which is a fault of the program and not of the kernel.
 */
void *g_mox_door;

void _start(void *door, long argc, char *const argv[], char *const envp[])
{
    g_mox_door = door;
    start_set_environment(envp);

    exit(main((int)argc, argv, envp));

    for (;;) {
    }
}
#else
void _start(long argc, char *const argv[], char *const envp[])
{
    start_set_environment(envp);

    exit(main((int)argc, argv, envp));

    for (;;) {
    }
}
#endif /* MOX_ARCH_XTENSA */
