/*
 * Micro OS — signals, from the side of a program.
 *
 * The program is the proof of the five promises of the signal ABI on the one
 * machine with ring 3:
 *
 *   - a handler is a function of the program, entered by the kernel with the
 *     number of the signal, and the program returns from it;
 *   - SIG_DFL of SIGTERM ends a task, and the parent sees the status;
 *   - SIG_IGN leaves the program alive;
 *   - a signal cuts a waitpid() short with EINTR, and the handler runs before
 *     the program gets the answer;
 *   - kill() of a pid that names no task is an error, and the machine stays
 *     alive.
 *
 * The handler is a plain function: the kernel puts it on the stack of the
 * program and the restorer page of the port returns through sigreturn, so
 * the program needs no library and no assembly of its own. Every line below
 * is written by the program; the smoke test reads them in order.
 *
 * The program makes its calls through the vector 0x80 (`prog.h`), and twice
 * through the fast door of `syscall` with the helper below: the two doors of
 * the port build one frame, and the signal that is answered on the way out of
 * a call has to be answered the same way from either. The rest of the tree
 * takes the fast door (the library of a program), so a machine that runs this
 * program and, say, `/bin/ls` exercises both.
 */
#include "prog.h"

/*
 * One call through the instruction `syscall`, which the port points at its
 * fast entry with IA32_LSTAR: RCX and R11 belong to the instruction and are
 * clobbered by it, exactly as the library of a program declares them.
 */
static long fast_call(long number, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long rax __asm__("rax") = number;
    register long rdi __asm__("rdi") = a0;
    register long rsi __asm__("rsi") = a1;
    register long rdx __asm__("rdx") = a2;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;

    __asm__ volatile("syscall"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");

    return rax;
}

static volatile int g_int_runs;
static volatile int g_last_int;

static void on_int(int sig)
{
    g_int_runs++;
    g_last_int = sig;

    prog_puts("PROGRAM: sigint handler, signal ");
    prog_put_dec((unsigned long)sig);
    prog_puts("\n");
}

int signals_main(int argc, char *const argv[])
{
    int status = -1;
    long r;
    long pid;

    (void)argc;
    (void)argv;

    prog_puts("PROGRAM: signals starts, pid ");
    prog_put_dec((unsigned long)prog_getpid());
    prog_puts("\n");

    /* 1. A handler of the program, entered by the kernel. */
    g_int_runs = 0;
    r = prog_signal(KSYS_SIGINT, on_int);
    if (r < 0) {
        prog_puts("PROGRAM: signal failed\n");
        prog_exit(1);
        return 1;
    }

    r = prog_kill(prog_getpid(), KSYS_SIGINT);
    prog_puts("PROGRAM: self SIGINT returned ");
    prog_put_dec((unsigned long)(r < 0 ? -r : r));
    prog_puts(", handler runs ");
    prog_put_dec((unsigned long)g_int_runs);
    prog_puts("\n");

    if (r < 0 || g_int_runs != 1 || g_last_int != KSYS_SIGINT) {
        prog_puts("PROGRAM: the handler did not run\n");
        prog_exit(1);
        return 1;
    }

    /*
     * 1b. The same signal through the fast door: the frame of a call is one
     * frame whether the program came in with `int $0x80` or with `syscall`,
     * and a handler the kernel enters on the way out of the second is the
     * proof that the fast return writes the fields the handler frame needs.
     */
    g_int_runs = 0;
    r = fast_call(KSYS_KILL, (long)prog_getpid(), KSYS_SIGINT, 0, 0, 0, 0);
    prog_puts("PROGRAM: self SIGINT through syscall returned ");
    prog_put_dec((unsigned long)(r < 0 ? -r : r));
    prog_puts(", handler runs ");
    prog_put_dec((unsigned long)g_int_runs);
    prog_puts("\n");

    if (r < 0 || g_int_runs != 1 || g_last_int != KSYS_SIGINT) {
        prog_puts("PROGRAM: the handler of the fast door did not run\n");
        prog_exit(1);
        return 1;
    }

    /* 2. SIG_IGN: the signal is thrown away and the program lives. */
    r = prog_signal(KSYS_SIGTERM, (void (*)(int))KSYS_SIG_IGN);
    if (r < 0) {
        prog_puts("PROGRAM: signal ignore failed\n");
        prog_exit(1);
        return 1;
    }

    r = prog_kill(prog_getpid(), KSYS_SIGTERM);
    prog_puts("PROGRAM: ignored SIGTERM, kill ");
    prog_put_dec((unsigned long)(r < 0 ? -r : r));
    prog_puts(", still alive\n");

    if (r < 0) {
        prog_exit(1);
        return 1;
    }

    /* 3. SIG_DFL of SIGTERM: the child ends with 128 + 15. */
    pid = prog_fork();
    if (pid == 0) {
        /* The dispositions travel with the fork: the child of this test has
         * to answer TERM by itself, not with the ignore of the parent. */
        (void)prog_signal(KSYS_SIGTERM, (void (*)(int))KSYS_SIG_DFL);

        for (;;) {
            (void)prog_sleep(5);
        }
    }

    if (pid < 0) {
        prog_puts("PROGRAM: fork failed\n");
        prog_exit(1);
        return 1;
    }

    /* The child runs at the next tick; the signal waits for it in any case,
     * because the mark belongs to the task and not to the moment. */
    (void)prog_sleep(5);
    r = prog_kill(pid, KSYS_SIGTERM);
    if (r < 0) {
        prog_puts("PROGRAM: kill of the child failed\n");
        prog_exit(1);
        return 1;
    }

    r = prog_waitpid(pid, &status, 0);
    prog_puts("PROGRAM: term child waited ");
    prog_put_dec((unsigned long)(r < 0 ? -r : r));
    prog_puts(", status ");
    prog_put_dec((unsigned long)status);
    prog_puts("\n");

    if (status != 128 + KSYS_SIGTERM) {
        prog_exit(1);
        return 1;
    }

    /* 4. A signal cuts a wait short: the child sends the SIGINT to its
     * parent a little after the parent has entered waitpid(). */
    g_int_runs = 0;
    r = prog_signal(KSYS_SIGINT, on_int);
    if (r < 0) {
        prog_exit(1);
        return 1;
    }

    pid = prog_fork();
    if (pid == 0) {
        long parent = prog_getppid();

        (void)prog_signal(KSYS_SIGTERM, (void (*)(int))KSYS_SIG_DFL);
        (void)prog_sleep(10);
        (void)prog_kill(parent, KSYS_SIGINT);

        for (;;) {
            (void)prog_sleep(5);
        }
    }

    if (pid < 0) {
        prog_puts("PROGRAM: fork failed\n");
        prog_exit(1);
        return 1;
    }

    status = -1;
    r = fast_call(KSYS_WAITPID, pid, (long)&status, 0, 0, 0, 0);
    prog_puts("PROGRAM: wait returned ");
    prog_put_dec((unsigned long)(r < 0 ? -r : r));
    prog_puts(", handler runs ");
    prog_put_dec((unsigned long)g_int_runs);
    prog_puts("\n");

    if (r != -4 || g_int_runs != 1) {
        (void)prog_kill(pid, KSYS_SIGTERM);
        (void)prog_waitpid(pid, &status, 0);
        prog_exit(1);
        return 1;
    }

    /* The interrupted child is still there: the wait did not take it. */
    (void)prog_kill(pid, KSYS_SIGTERM);
    (void)prog_waitpid(pid, &status, 0);

    /* 5. A pid that names no task, and a signal that does not exist. */
    r = prog_kill(9999, KSYS_SIGTERM);
    prog_puts("PROGRAM: kill ghost returned ");
    prog_put_dec((unsigned long)(r < 0 ? -r : r));
    prog_puts("\n");

    if (r != -3) {
        prog_exit(1);
        return 1;
    }

    r = prog_kill(prog_getpid(), 99);
    prog_puts("PROGRAM: kill signal 99 returned ");
    prog_put_dec((unsigned long)(r < 0 ? -r : r));
    prog_puts("\n");

    if (r != -22) {
        prog_exit(1);
        return 1;
    }

    /*
     * 6. The tick of the timer is the other point of delivery: the child
     * below never makes a call - it spins on a flag - so the handler can
     * only run when the tick interrupts it and the port puts the frame of
     * the signal on its stack.
     */
    g_int_runs = 0;
    pid = prog_fork();
    if (pid == 0) {
        (void)prog_signal(KSYS_SIGINT, on_int);

        for (;;) {
            if (g_int_runs > 0) {
                prog_exit(0);
            }
        }
    }

    if (pid < 0) {
        prog_puts("PROGRAM: fork failed\n");
        prog_exit(1);
        return 1;
    }

    (void)prog_sleep(20);
    (void)prog_kill(pid, KSYS_SIGINT);

    status = -1;
    r = prog_waitpid(pid, &status, 0);
    prog_puts("PROGRAM: tick handler, waited ");
    prog_put_dec((unsigned long)(r < 0 ? -r : r));
    prog_puts(", status ");
    prog_put_dec((unsigned long)status);
    prog_puts("\n");

    if (status != 0) {
        prog_exit(1);
        return 1;
    }

    prog_puts("PROGRAM: signals done\n");
    prog_exit(0);
    return 0;
}
