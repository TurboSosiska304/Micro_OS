/*
 * Micro OS — two programs that compute in floating point at the same time.
 *
 * This is the gate of the state of the XMM registers and of the x87 unit
 * (docs/debts.md, K9). The parent and the children are tasks of one machine,
 * and each of them keeps two doubles in registers across a loop that lasts
 * for many quanta of the scheduler. The loop multiplies by two and by one
 * half, which is exact: whatever the arithmetic does, the value of a register
 * is the value it started with, so the digits of the answer are a statement
 * about the registers and not about rounding.
 *
 * A kernel that switches the general registers and the address space but not
 * the floating point state lets the tasks read each other's registers: the
 * accumulator of one becomes the value another task had, and the line it
 * prints is that value. The numbers below are what the computing tasks have
 * to say when each of them keeps its own registers:
 *
 *   parent  start 1.0 -> 1.000000000000 1.000000000000
 *   child 3 start 3.0 -> 3.000000000000 0.333333333333
 *   child 7 start 7.0 -> 7.000000000000 0.142857142857
 *
 * The second number of a line is 1/start, which the scaling of the loop
 * returns exactly; the twelve digits are the formatter of the library
 * (ULIBC_WITH_FLOAT), so a wrong digit is a wrong register and not a text.
 *
 * The children are two and not one on purpose: the machine of the smoke test
 * of two cores has two processors, and three tasks make at least one core
 * carry two of them. The loop is long enough (about a tenth of a second on
 * the emulated machine) that every pair is preempted many times.
 *
 * The last child is the gate of the other half of the contract: it puts the
 * unit into a state that is not the one after fninit (a value on the x87
 * stack, another control word, another rounding mode of MXCSR) and then
 * calls execve of this same image with the argument `state`. A new image has
 * to begin clean, so the line is the default control word 037f and the
 * default MXCSR 1f80; a kernel that carried the registers across the exec
 * would print the values the old image left.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * The work of one task. The two factors are volatile so that the compiler
 * cannot fold the four multiplications of an iteration into one, and the
 * accumulators are ordinary doubles so that they stay in the XMM registers of
 * the loop - a volatile accumulator would live in memory and the test would
 * prove nothing about the registers.
 */
#define FPU_ITERATIONS 3000000u

static void compute(double start, double *x_out, double *y_out)
{
    volatile double two = 2.0;
    volatile double half = 0.5;
    double x = start;
    double y = 1.0 / start;
    unsigned i;

    for (i = 0; i < FPU_ITERATIONS; i++) {
        x = x * two;
        x = x * half;
        y = y * half;
        y = y * two;
    }

    *x_out = x;
    *y_out = y;
}

static void report(const char *who, double start)
{
    double x;
    double y;

    compute(start, &x, &y);
    printf("FPU: %s %.12f %.12f\n", who, x, y);
}

/* The control word of the x87 unit and the control of SSE, as they are now. */
static void report_state(void)
{
    unsigned short control;
    unsigned int mxcsr;

    __asm__ volatile("fnstcw %0" : "=m"(control));
    __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
    printf("FPU: state %04x %04x\n", (unsigned)control, (unsigned)mxcsr);
}

/*
 * A state that is not the one of a fresh unit: a number on the x87 stack,
 * the single precision and the rounding toward zero in the control word, and
 * the rounding toward zero in MXCSR. Every exception stays masked, so the
 * state is odd and not one that would trap the next instruction.
 */
static void dirty_state(void)
{
    unsigned short control = 0x0F7Fu;
    unsigned int mxcsr = 0x7F80u;

    __asm__ volatile("fldpi");
    __asm__ volatile("fldcw %0" : : "m"(control));
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
}

static void exec_child(void)
{
    dirty_state();

    execvp("/bin/fpu2", (char *const[]){"fpu2", "state", NULL});

    /* Only a failed exec reaches this line. */
    printf("FPU: the exec of the state failed\n");
    exit(1);
}

int main(int argc, char *argv[])
{
    pid_t first;
    pid_t second;
    pid_t third;
    int status;
    /*
     * Volatile because fork() returns twice: the compiler has to keep the
     * count in memory across the second call, or the child's copy of the
     * register is the one the parent goes on with.
     */
    volatile int failures = 0;

    /* The image the exec child becomes: the state of a unit that has just
     * been reset, and not the one the child left behind. */
    if (argc > 1 && strcmp(argv[1], "state") == 0) {
        report_state();
        return 0;
    }

    printf("FPU: three tasks with a double at once\n");

    first = fork();
    if (first == 0) {
        report("child 3", 3.0);
        exit(0);
    }

    second = fork();
    if (second == 0) {
        report("child 7", 7.0);
        exit(0);
    }

    third = fork();
    if (third == 0) {
        exec_child();
    }

    if (first < 0 || second < 0 || third < 0) {
        printf("FPU: fork failed\n");
        return 1;
    }

    /* The parent computes while the children do, which is the whole point. */
    report("parent", 1.0);

    if (waitpid(first, &status, 0) != first || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failures++;
    }

    if (waitpid(second, &status, 0) != second || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failures++;
    }

    if (waitpid(third, &status, 0) != third || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        failures++;
    }

    printf("FPU: all children ended, %d failure(s)\n", failures);

    return failures != 0;
}
