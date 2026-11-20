/*
 * Micro OS — what a program of the x86-64 port uses to talk to the kernel.
 *
 * A program is not linked against the kernel and has no library of its own:
 * the only door is the vector 0x80 of the table of the interrupts, and this
 * header is the shape of the door for a C compiler (ADR 0006). The numbers of
 * the calls are the ones of kernel/ksys.h and are not repeated here: a
 * program is compiled against the same header the kernel is, so a number can
 * never drift between the two sides.
 *
 * The convention is the one of Linux on this processor: the number of the
 * call in RAX, the arguments in RDI, RSI, RDX, R10, R8 and R9 (RCX and R11
 * are not used because the instruction syscall of a 64-bit machine takes
 * them for itself, and keeping one convention for both costs nothing), the
 * result in RAX, and a negative result is an error.
 *
 * The register variables are what pins the arguments to those registers: a
 * plain "r" constraint would let the compiler choose, and the kernel reads
 * the registers of the convention and nothing else. RCX, R11, the flags and
 * the memory are clobbered because the handler and the kernel behind it are
 * free to change them.
 */
#ifndef MICRO_OS_PORT_X86_64_PROGRAMS_PROG_H
#define MICRO_OS_PORT_X86_64_PROGRAMS_PROG_H

#include "kernel/ksys.h"

static inline long prog_call(long number, long a0, long a1, long a2, long a3, long a4, long a5)
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

/* The calls a program of this milestone makes, one line each. */

static inline long prog_write(int fd, const void *buf, unsigned long count)
{
    return prog_call(KSYS_WRITE, fd, (long)(unsigned long)buf, (long)count, 0, 0, 0);
}

static inline long prog_getpid(void)
{
    return prog_call(KSYS_GETPID, 0, 0, 0, 0, 0, 0);
}

static inline long prog_uptime(void)
{
    return prog_call(KSYS_UPTIME, 0, 0, 0, 0, 0, 0);
}

static inline long prog_sleep(long ms)
{
    return prog_call(KSYS_SLEEP, ms, 0, 0, 0, 0, 0);
}

/*
 * The end of a program of this port: the kernel takes it down and returns to
 * the call that entered it, so this call does not come back. A program that
 * returned with `ret` instead would pop the zero at the top of its stack and
 * fault on address zero; the port would recover, but the status would be the
 * fault of a mistake and not the number the program chose.
 */
static inline long prog_exit(int code)
{
    return prog_call(KSYS_EXIT, code, 0, 0, 0, 0, 0);
}

/* The length of the line, without the library a program does not have. */
static inline long prog_strlen(const char *text)
{
    long length = 0;

    while (text[length] != '\0') {
        length++;
    }

    return length;
}

/*
 * The line, without the carriage return: the console of the port is the one
 * that turns a newline into the pair the terminal expects, exactly as it does
 * for a line of the kernel. A program that wrote both would print a blank
 * line on a machine that had already done it.
 */
static inline long prog_puts(const char *text)
{
    return prog_write(1, text, (unsigned long)prog_strlen(text));
}

/* An unsigned number in decimal, one digit at a time, without a formatter. */
static inline void prog_put_dec(unsigned long value)
{
    char digits[20];
    int count = 0;

    if (value == 0) {
        (void)prog_write(1, "0", 1);
        return;
    }

    while (value > 0) {
        digits[count++] = (char)('0' + (int)(value % 10u));
        value /= 10u;
    }

    while (count > 0) {
        count--;
        (void)prog_write(1, &digits[count], 1);
    }
}

#endif /* MICRO_OS_PORT_X86_64_PROGRAMS_PROG_H */
