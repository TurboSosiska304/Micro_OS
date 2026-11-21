/*
 * Micro OS — the interrupts of the x86-64 port.
 *
 * The table of the interrupts lives in port_x86_64.c; this file owns the
 * first instructions of every handler, the part that has to run before C can
 * be called. There are two shapes here.
 *
 * An exception is a failure of the running code, and nothing is restored,
 * because the handler never returns: the processor pushes RIP, CS and
 * RFLAGS and, for some vectors, an error code. The stub adds the number of
 * the vector, so that both cases give C the same three numbers, and C says
 * what happened and stops the machine.
 *
 * An interrupt of the timer is a normal event: the handler does its work and
 * returns with iretq, so every register it touches is saved around the call
 * of C. The kernel is built without the red zone and without SSE, so the
 * stack and the general registers are the whole of the state.
 */
.section .text, "ax", @progbits
.code64

/*
 * One stub per exception. The processor pushes RIP, CS and RFLAGS, and the
 * error code last, so it ends up on top of the frame; a vector without one
 * gets a zero pushed in its place. Either way the frame at the common tail
 * is: vector, error, RIP.
 */
.macro isr_exception number
.global isr\number
.type isr\number, @function
isr\number:
    pushq $0
    pushq $\number
    jmp isr_exception_common
.size isr\number, . - isr\number
.endm

/* The same for the vectors that carry an error code of their own: the code
 * is already on the stack, so the stub adds only the number. */
.macro isr_exception_err number
.global isr\number
.type isr\number, @function
isr\number:
    pushq $\number
    jmp isr_exception_common
.size isr\number, . - isr\number
.endm

isr_exception 0
isr_exception 1
isr_exception 2
isr_exception 3
isr_exception 4
isr_exception 5
isr_exception 6
isr_exception 7
isr_exception_err 8
isr_exception 9
isr_exception_err 10
isr_exception_err 11
isr_exception_err 12
isr_exception_err 13
isr_exception_err 14
isr_exception 15
isr_exception 16
isr_exception_err 17
isr_exception 18
isr_exception 19
isr_exception_err 20
isr_exception_err 21
isr_exception 22
isr_exception 23
isr_exception 24
isr_exception 25
isr_exception 26
isr_exception 27
isr_exception 28
isr_exception_err 29
isr_exception_err 30
isr_exception 31

/*
 * The common tail of the exceptions. The registers of the moment are saved
 * because one exception returns: the first write into a page a fork made
 * read-only is handled, and the instruction has to be run again with every
 * register it had. A fault of the kernel or a fault of the program of ring 3
 * takes the machine or the task down, and the frame is never used.
 *
 * The frame below the saved registers is the one the processor and the stub
 * built: vector, error, RIP, CS (and RSP, SS when the fault came from ring
 * 3). The thread pointer has to come back to the kernel before any C runs: a
 * program of ring 3 leaves IA32_FS_BASE at zero, and the handler may log or
 * set an errno.
 */
.type isr_exception_common, @function
isr_exception_common:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %rbp

    movq %rsp, %rbp

    /* The ABI wants the stack of a call aligned to 16 bytes, and an
     * interrupt may have arrived at any alignment. */
    andq $-16, %rsp
    call port_x86_64_tls_kernel
    movq %rbp, %rsp

    movq 80(%rbp), %rdi  /* the vector */
    movq 88(%rbp), %rsi  /* the error code, or the zero of the stub */
    movq 96(%rbp), %rdx  /* the instruction that was running */
    movq 104(%rbp), %rcx /* the code selector of the moment */

    andq $-16, %rsp
    call port_x86_64_exception
    movq %rbp, %rsp

    /*
     * The only way back into the interrupted code is a page that was copied
     * for a write. A frame of ring 3 must leave the thread pointer at zero,
     * because the block of the kernel is not mapped there; a frame of ring 0
     * keeps the pointer it had.
     */
    movq 104(%rbp), %rax
    testb $3, %al
    jz 1f
    andq $-16, %rsp
    call port_x86_64_tls_user
    movq %rbp, %rsp
1:
    popq %rbp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax

    addq $16, %rsp /* the vector and the error code, not part of the frame */
    iretq
.size isr_exception_common, . - isr_exception_common

/*
 * An interrupt of the hardware is a normal event: the handler does its work
 * and returns with iretq, so every register it touches is saved around the
 * call of C. The kernel is built without the red zone and without SSE, so
 * the stack and the general registers are the whole of the state.
 *
 * The stub is the same for every line: the processor has already pushed the
 * frame it restores with iretq, and the body of C is the only thing that
 * changes from one line to the next.
 */
.macro isr_irq name, handler
.global \name
.type \name, @function
\name:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %rbp

    movq %rsp, %rbp /* the saved stack, for the way back */

    /* The C side is told where the frame is: the tick looks at the selector
     * of the interrupted code and may hand a signal to a program of ring 3. */
    movq %rbp, %rdi
    andq $-16, %rsp
    call \handler
    movq %rbp, %rsp

    /*
     * A tick that interrupted a program returns to ring 3, and the thread
     * pointer of the kernel must not follow it: the program has no address
     * for the block, and IA32_FS_BASE zero is what it had before the tick.
     * The frame's CS is the selector of the interrupted code (rbp + 88: the
     * ten saved registers, then RIP and CS).
     */
    movq 88(%rbp), %rax
    testb $3, %al
    jz 1f
    andq $-16, %rsp
    call port_x86_64_tls_user
    movq %rbp, %rsp
1:
    popq %rbp
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rax
    iretq
.size \name, . - \name
.endm

/* The timer: the clock of the kernel. */
isr_irq isr_irq0, port_x86_64_irq_tick

/* The keyboard: the byte of the PS/2 controller that raised the line. */
isr_irq isr_irq1, port_x86_64_irq_keyboard

/*
 * The call of a system call: `int $0x80` from the program.
 *
 * The program is ring 3 code with page tables of its own (ADR 0007), so the
 * processor takes the stack named by RSP0 of the TSS: the handler runs on a
 * kernel stack that belongs to the program and not on the stack of the
 * program, which ring 3 could have pointed anywhere.
 *
 * The registers of the convention are the ones of the kernel call and of
 * Linux: the number in RAX, six arguments in RDI, RSI, RDX, R10, R8 and R9,
 * the result in RAX. Everything the caller may lose across a C call is saved
 * here - the nine registers below plus RBP, which the frame uses - because
 * the C side is free to clobber them and the program is not compiled against
 * this handler.
 *
 * The interrupts are turned on once the state is on the stack, and not
 * before: the whole point of the gate is that a call may sleep or yield, and
 * the scheduler of the port decides by the interrupt flag. The iretq that
 * ends the stub restores the flags of the program, so the flag is on for the
 * length of the call and off again in the code that made it.
 */
.global isr_syscall
.type isr_syscall, @function
isr_syscall:
    pushq %rax /* the number of the call, then its result */
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %r11
    pushq %rcx
    pushq %rbp

    movq %rsp, %rbp /* the saved frame, for the way back and for its fields */

    /*
     * The program runs with the thread pointer of ring 3 (zero); the call of
     * the kernel is about to use its own. The ABI wants the stack aligned
     * before a call, so the register is set with the frame already safe.
     */
    andq $-16, %rsp
    call port_x86_64_tls_kernel
    movq %rbp, %rsp

    sti             /* a call may sleep, yield or wait for the console */

    /*
     * The frame, as the pushes above left it: 0 is RBP, then RCX, R11, R9,
     * R8, R10, RDX, RSI, RDI and finally RAX at 72. The order of the pushes
     * is the order of the pops read backwards (see the tail of the stub).
     *
     * The C call needs the stack 16 bytes aligned and one slot for the
     * seventh argument: ksys_call() takes the number and the six of the
     * convention, and a machine of this ABI passes the last one on the
     * stack.
     */
    andq $-16, %rsp
    subq $16, %rsp
    movq 24(%rbp), %rax /* the saved R9: the sixth argument */
    movq %rax, (%rsp)
    movq 72(%rbp), %rdi /* the number of the call */
    movq 64(%rbp), %rsi /* the first argument */
    movq 56(%rbp), %rdx
    movq 48(%rbp), %rcx
    movq 40(%rbp), %r8
    movq 32(%rbp), %r9
    call ksys_call
    movq %rbp, %rsp
    movq %rax, 72(%rbp) /* the result, where the iretq finds it */

    /*
     * The way back is ring 3, where the kernel's TLS block is not mapped.
     * Setting the pointer to zero here is what makes the next call of the
     * program enter a kernel with its own errno and not with a page fault.
     * A call that ended the program never reaches this instruction.
     */
    andq $-16, %rsp
    call port_x86_64_tls_user
    movq %rbp, %rsp

    popq %rbp
    popq %rcx
    popq %r11
    popq %r9
    popq %r8
    popq %r10
    popq %rdx
    popq %rsi
    popq %rdi
    popq %rax
    iretq
.size isr_syscall, . - isr_syscall

/*
 * A vector nobody claimed. It cannot say which one it was, because it has
 * no stub of its own, but a sentence beats a jump to a zero.
 */
.global isr_unexpected
.type isr_unexpected, @function
isr_unexpected:
    movq %rsp, %rbp
    andq $-16, %rsp
    call port_x86_64_unexpected

1:
    cli
    hlt
    jmp 1b
.size isr_unexpected, . - isr_unexpected

/*
 * The addresses of the 32 exception stubs, so that the C side can fill the
 * table of the interrupts without naming every symbol of this file.
 */
.section .rodata, "a", @progbits
.align 8
.global port_isr_exception_table
.type port_isr_exception_table, @object
port_isr_exception_table:
    .quad isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .quad isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
.size port_isr_exception_table, . - port_isr_exception_table

.section .note.GNU-stack, "", @progbits
