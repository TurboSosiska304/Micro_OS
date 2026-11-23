/*
 * Micro OS — the cost of the door: one program that takes both.
 *
 * A measurement of the crossing needs the two instructions in the same
 * program, built by the same compiler, run on the same machine: `int $0x80`
 * through the gate of the table of the interrupts and `syscall` through the
 * fast door of IA32_LSTAR. The program calls `getpid` - the cheapest call the
 * kernel has, so what is left of the number is the crossing - and prints the
 * count and the time of the loop.
 *
 *     exec /bin/bench int 200000
 *     exec /bin/bench fast 200000
 *
 * The clock is the millisecond of the kernel (KSYS_UPTIME): a run of a
 * second is a number with an error of a tenth of a percent, which is what a
 * comparison of two doors needs.
 */
#include "prog.h"

/* The call through the gate of the table of the interrupts. */
static long call_int(long number, long a0, long a1, long a2, long a3, long a4, long a5)
{
    register long rax __asm__("rax") = number;
    register long rdi __asm__("rdi") = a0;
    register long rsi __asm__("rsi") = a1;
    register long rdx __asm__("rdx") = a2;
    register long r10 __asm__("r10") = a3;
    register long r8 __asm__("r8") = a4;
    register long r9 __asm__("r9") = a5;

    __asm__ volatile("int $0x80"
                     : "+r"(rax)
                     : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "cc", "memory");

    return rax;
}

/* And through the fast door of the port. */
static long call_fast(long number, long a0, long a1, long a2, long a3, long a4, long a5)
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

static int word_is(const char *word, const char *text)
{
    while (*word != '\0' && *text != '\0' && *word == *text) {
        word++;
        text++;
    }

    return *word == '\0' && *text == '\0';
}

static unsigned long count_arg(const char *text)
{
    unsigned long value = 0;

    while (*text >= '0' && *text <= '9') {
        value = value * 10u + (unsigned long)(*text - '0');
        text++;
    }

    return value;
}

int bench_main(int argc, char *const argv[], char *const envp[])
{
    unsigned long count = 200000u;
    unsigned long before;
    unsigned long after;
    unsigned long i;
    long pid = 0;

    (void)envp;

    if (argc > 2 && argv[2] != NULL) {
        count = count_arg(argv[2]);
    }

    if (argc > 1 && argv[1] != NULL && word_is(argv[1], "int")) {
        before = (unsigned long)prog_uptime();
        for (i = 0; i < count; i++) {
            pid = call_int(KSYS_GETPID, 0, 0, 0, 0, 0, 0);
        }
        after = (unsigned long)prog_uptime();

        prog_puts("bench: int $0x80, ");
    } else {
        before = (unsigned long)prog_uptime();
        for (i = 0; i < count; i++) {
            pid = call_fast(KSYS_GETPID, 0, 0, 0, 0, 0, 0);
        }
        after = (unsigned long)prog_uptime();

        prog_puts("bench: syscall, ");
    }

    prog_put_dec(count);
    prog_puts(" calls in ");
    prog_put_dec(after - before);
    prog_puts(" ms, pid ");
    prog_put_dec((unsigned long)pid);
    prog_puts("\n");

    prog_exit(0);

    return 0;
}
