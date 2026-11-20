/*
 * Micro OS — the x86-64 port: running a program.
 *
 * The window starts at 32 MiB. The kernel is linked at 16 MiB, the heap of
 * the port is cut below the window (see heap_init in port_x86_64.c), and the
 * identity map of the loader covers the first gigabyte, so this address means
 * the same thing to an image and to the processor.
 *
 * Two duties meet in this file: placing the image (the copy of the text and
 * the data, the zeroed bss) and the transfer of control itself. The second
 * one is the interesting half. Before the jump the caller is ordinary kernel
 * code on the stack of the boot; after it there is no task of the kernel
 * under the program, no return address on its stack, and a tick of the
 * scheduler would take the CPU away from code that never asked to give it.
 * The state of the kernel is therefore saved by hand, the interrupts are
 * turned off, and the stack of the program is one of its own.
 *
 * The program is not cut off from the kernel, though: it calls the system
 * through `int $0x80` (src/interrupts.S), and the stub of the vector turns
 * the interrupts on for the length of the call, so a call that sleeps or
 * yields really waits. The program itself still runs with the interrupts
 * off - a program is not a task of the scheduler yet - and comes back to the
 * port with a `ret` and a status in RAX.
 */
#include <string.h>

#include "kernel/log.h"
#include "kernel/port/exec.h"
#include "port_x86_64.h"

/*
 * The window of the programs: the image is placed at its start and the stack
 * of the program is cut from its top. Sixteen megabytes is far more than a
 * program of this system needs, and the whole of it is inside the memory the
 * machine has (QEMU is started with 256 MB) and inside the identity map of
 * the loader.
 */
#define PORT_EXEC_BASE 0x02000000u
#define PORT_EXEC_LIMIT 0x03000000u

/*
 * The stack of the program is not the stack of the kernel and not the stack
 * of a task: a program written in C pushes a lot, and growing down from the
 * stack of the boot would walk into the .bss of the kernel one frame at a
 * time. Sixty-four kilobytes is the budget of a program here, and the image
 * has to leave that much room at the end of the window.
 */
#define PORT_EXEC_STACK_SIZE (64u * 1024u)

/* A program of this system has no use for more arguments than a shell line. */
#define PORT_EXEC_ARGC_MAX 64u

/*
 * The state of the kernel while the program runs. It is a variable of the
 * file and not a local of k_port_exec_enter() because the stub that receives
 * the program back is assembly at the level of the file: it finds the state
 * by a symbol, with an addressing mode relative to the instruction pointer.
 * One program at a time is the whole of this port's design, so one variable
 * is the right shape: the boot enters a program, the program returns, and
 * the variable is written again by the next one.
 */
k_u64_t g_port_exec_rsp;

/*
 * What the program left in RAX when it returned. The stub below stores it
 * before the stack of the kernel is back in place, because the C function
 * that called port_exec_jump() may not assume anything about a register the
 * ABI calls caller-saved; the variable is the one place the two sides of the
 * jump agree on, as g_port_exec_rsp is for the stack.
 */
k_u64_t g_port_exec_exit_code;

/*
 * void port_exec_jump(void *entry, k_u64_t stack_top, k_u64_t argc, char *const *argv)
 *
 * Enter the program at `entry` on a stack whose top is `stack_top`, with the
 * count and the vector of the arguments where the first instruction of a C
 * function finds them (RDI and RSI). The registers the ABI calls callee-saved
 * are pushed first: the program is code the kernel did not compile and is
 * free to clobber every one of them, while the C function that called this
 * one expects them back as they were.
 *
 * The address of the stub below goes onto the stack of the program, so a
 * `ret` at its end comes back here. The contract of the port says a program
 * does not return, and a program of the system does not: this path exists for
 * the demonstration of the milestone (kmain.c), which runs before the shell
 * exists and still has to leave the machine able to start it. A program that
 * halts instead never reaches the stub.
 *
 * Nothing here touches the stack of the kernel except its saved pointer: it
 * lives in a variable until the stub reads it.
 */
void port_exec_jump(void *entry, k_u64_t stack_top, k_u64_t argc, char *const *argv);

__asm__(".section .text, \"ax\", @progbits\n"
        ".globl port_exec_jump\n"
        ".type port_exec_jump, @function\n"
        "port_exec_jump:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, g_port_exec_rsp(%rip)\n"
        "    movq %rdi, %rax\n" /* the entry, before RDI carries argc */
        "    movq %rsi, %rsp\n" /* the stack of the program */
        "    movq %rdx, %rdi\n" /* argc */
        "    movq %rcx, %rsi\n" /* argv */
        "    leaq port_exec_exit(%rip), %rcx\n"
        "    pushq %rcx\n" /* where a ret of the program lands */
        "    cld\n"        /* the ABI starts with DF clear */
        "    xorl %ebp, %ebp\n"
        "    jmp *%rax\n"
        ".size port_exec_jump, . - port_exec_jump\n"
        "\n"
        /*
         * The program came back, through a ret or through anything else that
         * ends at the address above. The value in RAX is the status of the
         * program - the result of its last call, or the number a C entry
         * point returns - and it goes to the port before the stack of the
         * kernel hides it. The stack of the kernel and its callee-saved
         * registers are restored next, and the return goes back into
         * k_port_exec_enter() as if the call of port_exec_jump() had just
         * finished. The direction flag is cleared again: the program may have
         * set it, and the kernel copies with rep movsb.
         */
        "port_exec_exit:\n"
        "    movq %rax, g_port_exec_exit_code(%rip)\n"
        "    movq g_port_exec_rsp(%rip), %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    cld\n"
        "    ret\n");

bool k_port_exec_supported(void)
{
    return true;
}

void *k_port_exec_base(void)
{
    return (void *)(uintptr_t)PORT_EXEC_BASE;
}

int k_port_exec_enter(const struct k_exec_image *image, char *const argv[])
{
    k_u8_t *base = (k_u8_t *)(uintptr_t)PORT_EXEC_BASE;
    k_u64_t stack_top = (k_u64_t)PORT_EXEC_LIMIT;
    k_u64_t source;
    k_u64_t end;
    k_u64_t argc = 0;
    k_u64_t flags;

    if (image == NULL || image->data == NULL || image->text_size == 0) {
        return -1;
    }

    /*
     * The image has to be whole: the header, the text and the data have to be
     * inside the bytes the caller holds. k_exec_parse() has checked this for
     * an image that came through a file, but the structure can also be filled
     * by hand, and the port copies from it without asking again. The two
     * comparisons after the sum catch a wrap of the 64-bit counter, which is
     * how a header that lies about its sizes gets past the sum.
     */
    source = (k_u64_t)K_EXEC_HEADER_SIZE + image->text_size + image->data_size;
    if (source < image->text_size || source < image->data_size || source > image->size) {
        return -1;
    }

    /*
     * The entry is an offset from the start of the image and it points into
     * the text: the header is not code and the data is not executed.
     */
    if (image->entry < K_EXEC_HEADER_SIZE ||
        image->entry >= (k_u64_t)K_EXEC_HEADER_SIZE + image->text_size) {
        return -1;
    }

    if (image->bss_size > K_EXEC_BSS_MAX) {
        return -1;
    }

    /*
     * The image and the stack of the program share one window, so the image
     * has to end below the stack. This is also the check that keeps a program
     * out of the kernel: the window starts above the heap and above the image
     * of the kernel, and nothing of the kernel is copied there.
     */
    end = source + image->bss_size;
    if (end > (k_u64_t)(stack_top - PORT_EXEC_BASE) - PORT_EXEC_STACK_SIZE) {
        kerror("x86_64: the image (%u bytes) does not fit the window of the programs",
               (unsigned)end);
        return -1;
    }

    /*
     * The vector is not copied: the port hands the program the array the
     * caller has. The kernel passes none today (k_exec_run() has no way to
     * build one), and a program of this milestone does not look at it.
     */
    if (argv != NULL) {
        while (argc < PORT_EXEC_ARGC_MAX && argv[argc] != NULL) {
            argc++;
        }
    }

    /*
     * The rest is one step: the copy, the clearing and the jump happen with
     * the interrupts off. The ticks are the first thing the program itself
     * must not get, because while it runs there is no task to switch to and
     * a tick would take the CPU from code that cannot be resumed as a task;
     * the calls of the program turn them on for their own length (see
     * isr_syscall in src/interrupts.S). The copy belongs to the same step, so
     * that a program entered from a task never leaves half of a window for
     * the scheduler to switch into. The flag is the one the kernel found, and
     * it comes back with the return below.
     */
    flags = port_save_flags_and_cli();

    /*
     * The header is copied with the segments: the entry of the image is an
     * offset from the start of the image, so the bytes keep the shape they
     * have in the file and the program starts at base + entry.
     */
    memcpy(base, image->data, (k_size_t)source);

    if (image->bss_size != 0) {
        memset(base + source, 0, (k_size_t)image->bss_size);
    }

    /*
     * The status is set before the jump so that a port that comes back
     * without a program having written RAX at all still has a defined
     * answer: 0, "the program finished without a word".
     */
    g_port_exec_exit_code = 0;

    port_exec_jump(base + image->entry, stack_top, argc, argv);

    /*
     * Reached only because the program returned. The contract says a program
     * does not return: its last instruction is a halt, a jump into another
     * program or the end of the world. The demonstration program of kmain.c
     * ends with a ret on purpose, so the boot continues into the shell and
     * the machine of the smoke test still answers.
     *
     * The status goes to the kernel through k_exec_exit(): the port knows the
     * moment (it is the one that got control back), the kernel knows what to
     * do with the number (it is the answer of execve). The line in the log
     * says both, because a program that came back is a fact of the machine
     * and the first thing a person looking at the console wants to know.
     */
    port_restore_flags(flags);
    k_exec_exit((int)g_port_exec_exit_code);
    kinfo("x86-64: the program exited with %d", (int)(g_port_exec_exit_code & 0xFFu));

    return 0;
}
