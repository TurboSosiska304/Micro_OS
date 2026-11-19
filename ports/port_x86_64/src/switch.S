/*
 * Micro OS — the x86-64 port: the switch of the execution contexts.
 *
 * The kernel scheduler decides which task runs; the platform performs the
 * switch. This is the whole of it on this machine: the registers the System V
 * ABI calls callee-saved and the stack pointer. The caller-saved registers
 * belong to the caller by definition, and the compiler has already spilled
 * what it needs across the call of port_x86_64_switch(), so there is nothing
 * else to keep.
 *
 * There is no floating point and no SSE in this kernel: the build is
 * -mgeneral-regs-only, and the port never touches the FPU state.
 *
 * The red zone is not available either (-mno-red-zone): an interrupt may
 * overwrite the bytes under RSP at any moment, so everything of value is
 * pushed onto the stack proper.
 */
.section .text, "ax", @progbits

/*
 * void port_x86_64_switch(k_u64_t *old_rsp, k_u64_t new_rsp)
 *
 * Save the context of the caller, store its stack pointer where the caller
 * asked, load the stack pointer of the task that is next, restore its
 * registers and return — into the middle of the call that suspended *that*
 * task. The function returns twice in the life of a task: once in the task
 * that gave up the CPU and once, much later, in the task that gets it.
 *
 * The order of the pushes is the order of the pops read backwards: the
 * bottom of the frame is R15, the top is the return address, so a frame
 * built by hand for a task that has never run has exactly this layout:
 *
 *   [rsp+ 0] r15   [rsp+ 8] r14   [rsp+16] r13
 *   [rsp+24] r12   [rsp+32] rbx   [rsp+40] rbp
 *   [rsp+48] the address the ret jumps to
 *
 * port_x86_64_task.c builds that frame; the two files are one mechanism and
 * the layout above is their contract.
 */
.globl port_x86_64_switch
.type port_x86_64_switch, @function
port_x86_64_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi) /* *old_rsp: where this context stops */
    movq %rsi, %rsp   /* and here the next one continues */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
.size port_x86_64_switch, . - port_x86_64_switch

/*
 * The first instructions of every task.
 *
 * A task begins as the return of port_x86_64_switch() into this label, so
 * there is no caller to hand the arguments over in the usual registers. They
 * travel in the callee-saved ones instead, which the switch has just
 * restored from the frame: R12 is the entry point, R13 its argument, R14 the
 * RFLAGS the task runs with.
 *
 * The call is made with RSP 16 bytes aligned, as the ABI wants it at the
 * instruction: this label is entered the way a function is entered (RSP is
 * 8 mod 16), and the push of the return address would break that.
 */
.globl port_x86_64_task_bootstrap
.type port_x86_64_task_bootstrap, @function
port_x86_64_task_bootstrap:
    movq %r12, %rdi
    movq %r13, %rsi
    movq %r14, %rdx
    subq $8, %rsp
    call port_x86_64_task_trampoline

    /*
     * The C side never returns: it ends in k_port_task_exit(). If it does
     * return, the machine has nowhere to go and says so by stopping here
     * rather than by running through memory that is not a program.
     */
    cli
1:
    hlt
    jmp 1b
.size port_x86_64_task_bootstrap, . - port_x86_64_task_bootstrap

.section .note.GNU-stack, "", @progbits
