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

/* The offsets of the record of a core that the fast door reads: the numbers
 * are the ones the C uses (the header says what each of them is). */
#include "port_x86_64_asm.h"

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
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
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

    movq 120(%rbp), %rdi /* the vector */
    movq 128(%rbp), %rsi /* the error code, or the zero of the stub */
    movq 136(%rbp), %rdx /* the instruction that was running */
    movq 144(%rbp), %rcx /* the code selector of the moment */
    movq %rbp, %r8       /* the frame: registers and the iretq fields */

    andq $-16, %rsp
    call port_x86_64_exception
    movq %rbp, %rsp

    /*
     * The only way back into the interrupted code is a page that was copied
     * for a write. A frame of ring 3 must leave the thread pointer at zero,
     * because the block of the kernel is not mapped there; a frame of ring 0
     * keeps the pointer it had.
     */
    movq 144(%rbp), %rax
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
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15

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
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
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
     * The frame's CS is the selector of the interrupted code (rbp + 128: the
     * fifteen saved registers, then RIP and CS).
     */
    movq 128(%rbp), %rax
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
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    iretq
.size \name, . - \name
.endm

/* The timer: the clock of the kernel. */
isr_irq isr_irq0, port_x86_64_irq_tick

/* The keyboard: the byte of the PS/2 controller that raised the line. */
isr_irq isr_irq1, port_x86_64_irq_keyboard

/*
 * The pointer: the same controller, the second device (1.7.0). The body reads
 * the status byte and gives the byte to the mouse; the line is IRQ12.
 */
isr_irq isr_irq12, port_x86_64_irq_mouse

/*
 * The primary IDE channel, the sixth line of the slave: the end of a
 * bus-master transfer of the disk (ADR 0032). The handler acknowledges the
 * drive by reading its status and both chips of the 8259, and the wait of the
 * transfer reads the answer out of the engine of the channel.
 */
isr_irq isr_irq_ata, port_x86_64_irq_ata

/*
 * The virtio disk of the port (ADR 0037), on the line the configuration space
 * of its card names. The handler reads the interrupt status of the device,
 * which is what takes the line down, and acknowledges the 8259.
 */
isr_irq isr_irq_virtio_blk, port_x86_64_irq_virtio_blk

/*
 * The line of the controller of the USB (ADR 0040, M1 of 1.8): a message of
 * MSI into the local APIC, or the INTx pin of the function on a machine of
 * the 8259. The body clears the pending bit of the interrupter and tells the
 * console that the event ring has news.
 */
isr_irq isr_irq_xhci, port_x86_64_irq_xhci

/*
 * The three vectors of the 8259 that no device owns - the cascade and the two
 * spurious ones: the acknowledge of a request that has nothing behind it, the
 * price of a level-triggered line of the PCI bus. The handler acknowledges and
 * returns (src/port_x86_64.c has the whole story).
 */
isr_irq isr_irq_8259_spurious, port_x86_64_irq_8259_spurious

/*
 * The timer of a core that the 8259 does not reach: the local APIC of an
 * application processor (ADR 0012). The body is the tick of the port, and the
 * frame is the one of the interrupted code, like the frame of the PIT.
 */
isr_irq isr_lapic_timer, port_x86_64_irq_lapic_timer

/*
 * An inter-processor interrupt: another core asks this one to look at its run
 * queue. The body acknowledges the APIC and calls the hook of the scheduler.
 */
isr_irq isr_ipi, port_x86_64_irq_ipi

/*
 * The spurious vector: the APIC withdrew an interrupt before delivering it.
 * The body returns without an EOI, because there is no interrupt in service to
 * end, and the stub gives the interrupted code its registers back.
 */
isr_irq isr_spurious, port_x86_64_irq_spurious

/*
 * The fifteen registers of the frame, in the order that is the contract of
 * the file port_exec.c reads a frame by (PORT_FRAME_RBP .. PORT_FRAME_R15):
 * the last one pushed is the first word of the frame. The frame of a call is
 * 160 bytes of these and the five words of the crossing above them.
 */
.macro ksys_save_registers
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
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
.endm

/*
 * The body of a call, the same for both doors: the frame is at RBP and the
 * kernel is entered with the thread pointer of ring 3, the interrupts off and
 * the stack of the program.
 *
 * The program runs with the thread pointer of ring 3 (zero); the call of the
 * kernel is about to use its own. The ABI wants the stack aligned before a
 * call, so the register is set with the frame already safe.
 *
 * The interrupts are turned on once the state is on the stack, and not
 * before: the whole point of the crossing is that a call may sleep or yield,
 * and the scheduler of the port decides by the interrupt flag.
 *
 * The frame, as the pushes above left it: 0 is RBP, then RCX, R11, R9, R8,
 * R10, RDX, RSI, RDI and RAX at 72, then the callee-saved RBX, R12, R13, R14
 * and R15, and the five words of the crossing. The C call needs the stack 16
 * bytes aligned and one slot for the seventh argument: ksys_call() takes the
 * number and the six of the convention, and a machine of this ABI passes the
 * last one on the stack.
 */
.macro ksys_call_body
    andq $-16, %rsp
    call port_x86_64_tls_kernel
    movq %rbp, %rsp

    sti /* a call may sleep, yield or wait for the console */

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
    movq %rax, 72(%rbp) /* the result, where the way back finds it */

    /*
     * The way back is ring 3, where the kernel's TLS block is not mapped.
     * Setting the pointer to zero here is what makes the next call of the
     * program enter a kernel with its own errno and not with a page fault.
     * A call that ended the program never reaches this instruction.
     */
    andq $-16, %rsp
    call port_x86_64_tls_user
    movq %rbp, %rsp
.endm

/*
 * The call of a system call through the gate of the table of the interrupts:
 * `int $0x80` from a program that was built for it, and the restorer of a
 * signal handler (port_exec.c), which is the one caller the port writes.
 *
 * The program is ring 3 code with page tables of its own (ADR 0007), so the
 * processor takes the stack named by RSP0 of the TSS: the handler runs on a
 * kernel stack that belongs to the program and not on the stack of the
 * program, which ring 3 could have pointed anywhere. The five words of the
 * crossing are already on the stack when the stub starts; the fifteen
 * registers are pushed here, and the whole of the frame is the one the fast
 * door below builds by hand.
 *
 * The registers of the convention are the ones of the kernel call and of
 * Linux: the number in RAX, six arguments in RDI, RSI, RDX, R10, R8 and R9,
 * the result in RAX. Everything the caller may lose across a C call is saved
 * here - the nine registers below plus RBP, and the five callee-saved ones -
 * because the C side is free to clobber them and the program is not compiled
 * against this handler.
 */
.global isr_syscall
.type isr_syscall, @function
isr_syscall:
    ksys_save_registers
    jmp ksys_enter
.size isr_syscall, . - isr_syscall

/*
 * The call of a system call through the fast door: the instruction `syscall`,
 * which the IA32_LSTAR register of the core points at (src/syscall.c).
 *
 * The instruction is a jump and nothing else: it does not push a frame, it
 * does not change the stack and it does not read a descriptor. It leaves the
 * return address of the program in RCX, its flags in R11 and the stack where
 * the program had it, and it has already cleared the flags of IA32_FMASK - the
 * interrupt flag above all, so that nothing arrives before the frame exists.
 *
 * The two things it does not do are done here, in four instructions. The stack
 * of the program goes into the record of the core (one word, because the
 * frame needs it and a register is either an argument or the number of the
 * call). The stack of the kernel comes out of the same record: it is the
 * number gdt.c writes into RSP0 of the TSS, so both doors land on the same
 * stack, at the same address, and a frame built here is a frame the rest of
 * the port cannot tell from one the gate built.
 *
 * The five words of the crossing are pushed by hand in the order the
 * processor would have pushed them. RCX and R11 are their own slots as well:
 * the instruction keeps them, so the frame has no other value for the two, and
 * a signal frame saved out of it shows the address of the call and the flags
 * of the program - which is what a dump of the gate shows too.
 */
.global isr_syscall_fast
.type isr_syscall_fast, @function
isr_syscall_fast:
    movq %rsp, %gs:PORT_X86_64_CPU_USER_RSP
    movq %gs:PORT_X86_64_CPU_KERNEL_RSP, %rsp

    pushq $PORT_X86_64_USER_SS            /* SS: the ring 3 data selector */
    pushq %gs:PORT_X86_64_CPU_USER_RSP    /* RSP: the stack of the program */
    pushq %r11                            /* RFLAGS: the flags of the program */
    pushq $PORT_X86_64_USER_CS            /* CS: the ring 3 code selector */
    pushq %rcx                            /* RIP: the instruction after the call */
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %rbx
    pushq %rax
    pushq %rdi
    pushq %rsi
    pushq %rdx
    pushq %r10
    pushq %r8
    pushq %r9
    pushq %rcx /* the slot of RCX: the instruction keeps the address */
    pushq %r11 /* the slot of R11: the instruction keeps the flags */
    pushq %rbp
    movq %rsp, %rbp
.size isr_syscall_fast, . - isr_syscall_fast

/* Both frames are built; the call itself is the same. */
ksys_enter:
    ksys_call_body

/*
 * The way back decides between the two instructions of the return.
 *
 * SYSRET is the fast one: it takes the flags from R11 and the address from
 * RCX and turns ring 3 on, and it does not read the frame, so the four
 * instructions below are the whole of the return. It is also the one that
 * cannot be handed a broken address: an address that is not canonical faults
 * in ring 0 *after* the stack of the program is in RSP, which would leave the
 * handler of the fault on a stack the program chose. The frame of a sigreturn
 * comes from the memory of the program, so the check is not paranoia.
 *
 * Hence the check on the two words that matter. An address that is not
 * canonical, or a frame that is not a program's (CS is not the ring 3
 * selector), takes the iretq of the gate instead: the instruction reads the
 * whole frame including the selector, it faults with the stack of the kernel
 * still in the register, and the fault handler takes the task down the way it
 * does today.
 *
 * The interrupts are off from here to the instruction of the return: the
 * stack of the program is loaded into RSP before the one instruction that
 * uses it, and an interrupt taken in that window would push a frame of the
 * kernel on a stack the program chose.
 */
ksys_return:
    movq %rbp, %rsp
    movq 120(%rbp), %rax /* the RIP slot of the frame */
    movq %rax, %rdx
    shlq $16, %rdx /* the sign of the address into the top sixteen bits: */
    sarq $16, %rdx /* a value that survives it is canonical, and only then */
    cmpq %rax, %rdx
    jne ksys_return_iret
    cmpq $PORT_X86_64_USER_CS, 128(%rbp) /* the CS slot: a program, or the gate */
    jne ksys_return_iret

    cli
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
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15

    /* The frame now begins at RIP: 0 is the address, 16 the flags and 24 the
     * stack of the program. SYSRET takes the first two from registers. */
    movq (%rsp), %rcx
    movq 16(%rsp), %r11
    movq 24(%rsp), %rsp
    sysretq

/*
 * The slow way back: iretq, which reads the whole frame the processor built
 * (or the port did) and restores the segment registers and the stack of the
 * program from it. It is the return of `int $0x80` and the fallback of the
 * fast door.
 */
ksys_return_iret:
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
    popq %rbx
    popq %r12
    popq %r13
    popq %r14
    popq %r15
    iretq

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
