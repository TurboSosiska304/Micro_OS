/*
 * Micro OS — the x86-64 port: running a program.
 *
 * A program is a task of the scheduler now (K4). It has page tables of its
 * own, its own stack in the window of the programs and its own kernel stack,
 * and the processor drops to ring 3 before its first instruction: the kernel
 * is above it and out of reach, and the only way back in is `int $0x80`
 * through a gate that opens for ring 3.
 *
 * The window starts at 32 MiB and every page of it that the program may use
 * is backed by a physical page of the arena at 64 MiB. The kernel reaches the
 * bytes through the identity map, the program through its page tables, and an
 * address the program computes is not an address of the kernel. The pages
 * come from an allocator of frames instead of a fixed offset in the arena,
 * because two programs - a parent and the child of its fork - may run at the
 * same time.
 *
 * Two duties meet here: placing the image (the pages, the copy of text and
 * data, the zeroed bss, the arguments) and the crossing itself. The crossing
 * is the interesting half. A task of the port starts as the return of a
 * context switch; this file gives it a program and an iretq, and the task is
 * then a user of the machine. `exit` ends the task, a fault of ring 3 ends it
 * with the status 255, and the task that called execve waits for it the way
 * any parent waits for a child.
 */
#include <string.h>

#include "kernel/console.h"
#include "kernel/tty.h"
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/mm.h"
#include "kernel/port.h"
#include "kernel/port/exec.h"
#include "kernel/port/time.h"
#include "kernel/signal.h"
#include "kernel/task.h"
#include "port_x86_64.h"
#include "port_x86_64_task.h"

#define PAGE_SIZE 4096u

#define PORT_EXEC_BASE ((k_u64_t)PORT_X86_64_USER_BASE)
#define PORT_EXEC_LIMIT ((k_u64_t)PORT_X86_64_USER_LIMIT)
#define PORT_EXEC_PHYS ((k_u64_t)PORT_X86_64_USER_PHYS)
#define PORT_EXEC_WINDOW (PORT_EXEC_LIMIT - PORT_EXEC_BASE)

/*
 * The stack of the program is 64 KiB at the top of the window. The page just
 * below it is not mapped: a program that overruns the stack faults instead of
 * walking into its own data, and a fault of a program is a line on the
 * console instead of a broken machine.
 */
#define PORT_EXEC_STACK_SIZE PORT_X86_64_USER_STACK_SIZE
#define PORT_EXEC_STACK_OFFSET PORT_X86_64_USER_STACK_OFFSET
#define PORT_EXEC_GUARD_OFFSET PORT_X86_64_USER_GUARD_OFFSET

/*
 * The page of the restorer of a signal handler. It is one page below the
 * guard page of the stack, out of the way of the image and of the arguments,
 * and it holds a few instructions a handler returns into:
 *
 *     movl $KSYS_SIGRETURN, %eax
 *     int $0x80
 *     hlt
 *
 * The kernel maps it into every program, and a signal frame puts its address
 * on the stack as the return address of the handler. A program of ring 3
 * cannot build such a frame itself: only the kernel knows where the
 * interrupted instruction was and with which registers.
 */
#define PORT_EXEC_RESTORER_OFFSET PORT_X86_64_USER_RESTORER_OFFSET
#define PORT_EXEC_RESTORER (PORT_EXEC_BASE + PORT_EXEC_RESTORER_OFFSET)

static const k_u8_t g_restorer_code[] = {
    0xB8u, /* movl $imm32, %eax */
    (k_u8_t)(KSYS_SIGRETURN & 0xFFu),
    (k_u8_t)((KSYS_SIGRETURN >> 8) & 0xFFu),
    (k_u8_t)((KSYS_SIGRETURN >> 16) & 0xFFu),
    (k_u8_t)((KSYS_SIGRETURN >> 24) & 0xFFu),
    0xCDu, /* int $0x80 */
    0x80u,
    0xF4u, /* hlt, should the call ever come back */
};

/* A program of this system has no use for more arguments than a shell line. */
#define PORT_EXEC_ARGC_MAX 64u
#define PORT_EXEC_ARG_BYTES_MAX 1024u

/*
 * The environment of a program is copied the same way and bounded the same
 * way as its arguments: the kernel copies at most eight words and 512 bytes
 * before the port sees the image, and a direct caller of the port is held to
 * the same numbers. A word of the environment is a "NAME=VALUE" string.
 */
#define PORT_EXEC_ENVP_MAX 8u
#define PORT_EXEC_ENV_BYTES_MAX 512u

/*
 * How long the caller of execve waits for the program it started.
 *
 * The wait is bounded because the machine has no job control: a program that
 * hangs in the kernel holds the shell of the console, and nothing else can
 * take the line back. Ten seconds was the first guess, and it is a guess of
 * a machine whose files were kilobytes: a program of this port writes a file
 * of a large volume at about a megabyte a second, so a file of 128 MiB of a
 * 5 GiB disk (the gate of ADR 0029) needed a hundred times the budget. The
 * caller that gives up in the middle of a write does not stop the program
 * either: the shell reads the next line while the old program still writes,
 * and two writers of one file are a volume the check has to repair.
 *
 * Ten minutes is long enough for the slowest thing a person waits for on
 * this machine and short enough that a program stuck in a cycle of its own
 * gives the console back. Ctrl+C of the console, which this loop already
 * reads, stays the way out of a program that is merely slow.
 */
#define PORT_EXEC_WAIT_MS 600000u

/* The moment a program that ran past the wait above is given to answer the
 * signal that tells it to stop. */
#define PORT_EXEC_STOP_MS 1000u

/*
 * The frame of a signal on the stack of the program: the registers and the
 * iretq frame of the moment the signal arrived, so that sigreturn can put
 * the program back where it was. The magic says that the bytes are a frame
 * of this kernel and not something the program left on its stack.
 */
#define PORT_SIGNAL_MAGIC 0x5349474652414D45ull /* "SIGFRAME" */

struct port_signal_frame {
    k_u64_t magic;
    k_u64_t rbp;
    k_u64_t rcx;
    k_u64_t r11;
    k_u64_t r9;
    k_u64_t r8;
    k_u64_t r10;
    k_u64_t rdx;
    k_u64_t rsi;
    k_u64_t rdi;
    k_u64_t rax;
    k_u64_t rip;
    k_u64_t cs;
    k_u64_t rflags;
    k_u64_t rsp;
    k_u64_t ss;
};

/*
 * The places in the frame that the two stubs of the port leave on the kernel
 * stack of a program. The iretq fields and RAX are at the same place in both;
 * the other registers are not, because the stub of the system call and the
 * stub of an interrupt push them in different orders.
 */
#define PORT_FRAME_RBP 0u
#define PORT_FRAME_RCX 1u
#define PORT_FRAME_R11 2u
#define PORT_FRAME_R9 3u
#define PORT_FRAME_R8 4u
#define PORT_FRAME_R10 5u
#define PORT_FRAME_RDX 6u
#define PORT_FRAME_RSI 7u
#define PORT_FRAME_RDI 8u
#define PORT_FRAME_RAX 9u
#define PORT_FRAME_RBX 10u
#define PORT_FRAME_R12 11u
#define PORT_FRAME_R13 12u
#define PORT_FRAME_R14 13u
#define PORT_FRAME_R15 14u
#define PORT_FRAME_RIP 15u
#define PORT_FRAME_CS 16u
#define PORT_FRAME_RFLAGS 17u
#define PORT_FRAME_RSP 18u
#define PORT_FRAME_SS 19u

/* The name of the task a program runs in: the table keeps sixteen bytes. */
#define PORT_EXEC_TASK_NAME "program"

/*
 * The state of a program between the call of execve and the first instruction
 * of ring 3. It lives in the heap of the kernel because the task is scheduled
 * after k_port_exec_enter() has gone to sleep; the body of the task copies
 * what it needs and gives the block back before it leaves the kernel.
 */
struct port_exec_start {
    k_u64_t space;
    k_u64_t entry;
    k_u64_t argc;
    k_u64_t argv;
    k_u64_t envp;
    k_u64_t user_rsp;
};

/*
 * void port_exec_enter_user(void *entry, k_u64_t argc, char *const *argv,
 *                           char *const *envp, k_u64_t user_rsp)
 *
 * Leave the kernel for ring 3 through an iretq frame: SS, RSP, RFLAGS, CS and
 * RIP, in the order the instruction pops them. The selectors are 0x23 (data,
 * DPL 3) and 0x1b (code, DPL 3) of the GDT of gdt.c.
 *
 * The program's first instruction sees the arguments of a C function - the
 * count in RDI, the vector of the arguments in RSI and the vector of the
 * environment in RDX, which is the entry ABI of this port (ADR 0016) - on a
 * stack whose top word is zero, so a `ret` that no program of the system
 * writes lands on address zero and faults instead of running through memory.
 * The kernel frames below are abandoned: the task resumes through RSP0 of the
 * TSS on its way back in.
 *
 * The five values arrive in RDI, RSI, RDX, RCX and R8 and are kept in the
 * callee-saved registers across the call of port_x86_64_tls_user(); R8 is the
 * stack of the program and it goes to RBX for the same reason. The registers
 * of the program are zeroed after the frame is pushed, so a program starts
 * with no state of the kernel anywhere - except the three arguments.
 */
void port_exec_enter_user(void *entry, k_u64_t argc, char *const *argv, char *const *envp,
                          k_u64_t user_rsp);
void port_exec_fork_return(k_u64_t *frame);
void port_exec_fork_child(void *arg);

__asm__(".section .text, \"ax\", @progbits\n"
        ".globl port_exec_enter_user\n"
        ".type port_exec_enter_user, @function\n"
        "port_exec_enter_user:\n"
        "    movq %rdi, %r12\n" /* the entry of the program */
        "    movq %rsi, %r13\n" /* argc */
        "    movq %rdx, %r14\n" /* argv */
        "    movq %rcx, %r15\n" /* envp */
        "    movq %r8, %rbx\n"  /* the stack of the program */
        "    andq $-16, %rsp\n"
        "    call port_x86_64_tls_user\n"
        "    pushq $0x23\n"  /* SS, ring 3 data */
        "    pushq %rbx\n"   /* RSP of the program */
        "    pushq $0x202\n" /* RFLAGS: interrupts on, the required bit 1 */
        "    pushq $0x1b\n"  /* CS, ring 3 code */
        "    pushq %r12\n"   /* RIP */
        "    movq %r13, %rdi\n"
        "    movq %r14, %rsi\n"
        "    movq %r15, %rdx\n"
        "    xorl %eax, %eax\n"
        "    xorl %ebx, %ebx\n"
        "    xorl %ecx, %ecx\n"
        "    xorl %ebp, %ebp\n"
        "    xorl %r8d, %r8d\n"
        "    xorl %r9d, %r9d\n"
        "    xorl %r10d, %r10d\n"
        "    xorl %r11d, %r11d\n"
        "    xorl %r12d, %r12d\n"
        "    xorl %r13d, %r13d\n"
        "    xorl %r14d, %r14d\n"
        "    xorl %r15d, %r15d\n"
        "    iretq\n"
        ".size port_exec_enter_user, . - port_exec_enter_user\n"
        "\n"
        /*
         * The way out of fork(): the child's kernel stack carries a copy of
         * the syscall frame of the parent, with zero where the result of the
         * call was. The tail below is the tail of isr_syscall - the thread
         * pointer of ring 3, the fifteen saved registers, the iretq - and it
         * restores every one of them from the frame, because a child has no
         * live registers to inherit: the frame is the whole of its state.
         */
        ".globl port_exec_fork_return\n"
        ".type port_exec_fork_return, @function\n"
        "port_exec_fork_return:\n"
        "    movq %rdi, %rbx\n" /* the frame; RBX is restored from it below */
        "    andq $-16, %rsp\n"
        "    call port_x86_64_tls_user\n"
        "    movq %rbx, %rsp\n"
        "    popq %rbp\n"
        "    popq %rcx\n"
        "    popq %r11\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %r10\n"
        "    popq %rdx\n"
        "    popq %rsi\n"
        "    popq %rdi\n"
        "    popq %rax\n"
        "    popq %rbx\n"
        "    popq %r12\n"
        "    popq %r13\n"
        "    popq %r14\n"
        "    popq %r15\n"
        "    testb $3, 8(%rsp)\n"
        "    jz 1f\n"
        "    cmpq $0x23, 32(%rsp)\n"
        "    je 1f\n"
        "    cmpq $0x3b, 32(%rsp)\n"
        "    je 1f\n"
        "    movq $0x23, 32(%rsp)\n"
        "1:\n"
        "    iretq\n"
        ".size port_exec_fork_return, . - port_exec_fork_return\n");

static k_size_t align_up(k_size_t value, k_size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static bool image_fits(const struct k_exec_image *image, k_size_t source, k_size_t end)
{
    K_UNUSED(image);

    if (end < source) {
        return false;
    }

    /* The image and its arguments stay below the restorer and the guard page
     * of the stack. */
    return end <= (k_size_t)PORT_EXEC_RESTORER_OFFSET;
}

/* Give `len` bytes of the window a frame of the arena, page by page. */
static bool space_map(k_u64_t space, k_size_t offset, k_size_t len, k_u32_t flags)
{
    k_size_t at;

    for (at = 0; at < len; at += PAGE_SIZE) {
        k_u64_t frame = port_x86_64_frame_alloc();

        if (frame == 0) {
            kerror("x86_64: no physical page for the program");
            return false;
        }

        if (port_x86_64_paging_map_user(space, PORT_EXEC_BASE + offset + at, frame, PAGE_SIZE,
                                        flags) != 0) {
            port_x86_64_frame_release(frame);
            kerror("x86_64: cannot map a page of the program");
            return false;
        }
    }

    return true;
}

/* Copy bytes into the memory of a program, across the pages of its window. */
static bool space_write(k_u64_t space, k_u64_t vaddr, const void *source, k_size_t len)
{
    const k_u8_t *bytes = (const k_u8_t *)source;

    while (len > 0) {
        void *physical = port_x86_64_paging_phys(space, vaddr);
        k_size_t in_page;
        k_size_t chunk;

        if (physical == NULL) {
            return false;
        }

        in_page = PAGE_SIZE - (k_size_t)(vaddr & (PAGE_SIZE - 1u));
        chunk = len < in_page ? len : in_page;

        memcpy(physical, bytes, chunk);

        bytes += chunk;
        vaddr += chunk;
        len -= chunk;
    }

    return true;
}

/* Clear bytes of the memory of a program, across the pages of its window. */
static bool space_zero(k_u64_t space, k_u64_t vaddr, k_size_t len)
{
    while (len > 0) {
        void *physical = port_x86_64_paging_phys(space, vaddr);
        k_size_t in_page;
        k_size_t chunk;

        if (physical == NULL) {
            return false;
        }

        in_page = PAGE_SIZE - (k_size_t)(vaddr & (PAGE_SIZE - 1u));
        chunk = len < in_page ? len : in_page;

        memset(physical, 0, chunk);

        vaddr += chunk;
        len -= chunk;
    }

    return true;
}

/*
 * The stack of the program that runs grew (ADR 0052). The room under the
 * first 64 KiB of the stack is a demand-paged region like the mappings of
 * kernel/mm.h: the fault handler asks here before it calls the fault a fault.
 *
 * The lowest page the stack owns is read from the tables and not kept in a
 * record, because the tables are the truth: a fork copies them and the child
 * inherits the stack its parent had, an `execve` makes them again, and a page
 * the program already took cannot be taken twice. The walk starts at the
 * floor and stops at the first present page, so it costs a page table read
 * per step of a stack that has not grown and one read after it has.
 *
 * The limit is the task's, not the port's: the room is the most a stack may
 * ever be, and KSYS_RLIMIT_MEMORY (`ulimit -m`) is the ceiling the task
 * carries. A program that lowers it below the room has a stack that stops
 * there, and the next frame is the fault of the program - a stack overflow is
 * not an answer of a call, it is the end of the task.
 */
bool port_x86_64_stack_grow(k_u64_t address)
{
    struct task *self = task_self();
    k_u64_t space = port_x86_64_task_space();
    k_u64_t floor = PORT_EXEC_BASE + (k_u64_t)PORT_X86_64_USER_STACK_FLOOR_OFFSET;
    k_u64_t bottom = 0;
    k_u64_t ceiling = (k_u64_t)PORT_X86_64_USER_STACK_SIZE + (k_u64_t)PORT_X86_64_USER_STACK_ROOM;
    k_u64_t at;

    if (space == 0 || address < floor || address >= PORT_EXEC_LIMIT) {
        return false;
    }

    address &= ~(k_u64_t)(PAGE_SIZE - 1u);

    for (at = floor; at < PORT_EXEC_LIMIT; at += PAGE_SIZE) {
        if (port_x86_64_paging_phys(space, at) != NULL) {
            bottom = at;
            break;
        }
    }

    /* No stack, or a page the stack already owns: not a fault of the guard. */
    if (bottom == 0 || address >= bottom) {
        return false;
    }

    /*
     * The ceiling of the task, applied to the stack as one object: the stack
     * of a program is the memory it holds in its frames, and a task that
     * asked for a smaller limit than the room gets the smaller stack.
     */
    if (self != NULL) {
        k_u64_t asked = 0;

        if (task_limit_get(self, KSYS_RLIMIT_MEMORY, &asked) == 0 && asked < ceiling) {
            ceiling = asked;
        }
    }

    if (ceiling > PORT_EXEC_LIMIT) {
        ceiling = PORT_EXEC_LIMIT;
    }

    if (address < PORT_EXEC_LIMIT - ceiling) {
        return false;
    }

    for (at = address; at < bottom; at += PAGE_SIZE) {
        if (port_x86_64_paging_map_page(space, at, PORT_X86_64_PAGE_WRITE) == NULL) {
            return false;
        }
    }

    /*
     * The line of the growth, for a frame that took more than one page. A
     * stack that grows is a promise of the map and this is the only place it
     * is visible - the program sees the same address it always had - but a
     * recursion that walks down a page at a time is how a stack is used and
     * not news: one line per page of a deep recursion would be a log nobody
     * reads. A frame of 128 KiB is one line.
     */
    if ((bottom - address) > PAGE_SIZE) {
        klog(K_LOG_LEVEL_INFO,
             "x86-64: the stack of the program (pid %u) grew by %u page(s) to 0x%08x%08x",
             (self != NULL) ? (unsigned)self->pid : 0u, (unsigned)((bottom - address) / PAGE_SIZE),
             (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu));
    }

    return true;
}

/*
 * The first code of the program in kernel mode. The task was made by
 * k_port_exec_enter() as an ordinary task of the scheduler; here it takes the
 * address space that belongs to it and drops to ring 3 on its user stack.
 */
static void port_exec_program_task(void *arg)
{
    struct port_exec_start *start = (struct port_exec_start *)arg;
    k_u64_t space;
    k_u64_t entry;
    k_u64_t argc;
    k_u64_t argv;
    k_u64_t envp;
    k_u64_t user_rsp;

    if (start == NULL) {
        task_exit(255);
        return;
    }

    space = start->space;
    entry = start->entry;
    argc = start->argc;
    argv = start->argv;
    envp = start->envp;
    user_rsp = start->user_rsp;
    k_port_free(start);

    port_x86_64_task_set_space(space);

    port_exec_enter_user((void *)(uintptr_t)entry, argc, (char *const *)(uintptr_t)argv,
                         (char *const *)(uintptr_t)envp, user_rsp);

    /* Not reached: the only way back from ring 3 is the call of exit. */
    task_exit(255);
}

/*
 * The first code of the child of a fork. Its kernel stack carries the frame
 * of the system call of the parent with zero as the result; the return below
 * is the tail of the stub of `int $0x80`, so the child lands at the
 * instruction after the call of fork with the answer of the child.
 */
void port_exec_fork_child(void *arg)
{
    port_exec_fork_return((k_u64_t *)(uintptr_t)arg);
}

bool k_port_exec_supported(void)
{
    return true;
}

int k_port_exec_arch(void)
{
    /* The bytes of this machine, and the pull of the iretq: x86-64. */
    return (int)K_EXEC_ARCH_X86_64;
}

void *k_port_exec_base(void)
{
    return (void *)(uintptr_t)PORT_EXEC_BASE;
}

/*
 * The line of a range that is not memory of the program (ADR 0052). The call
 * that asked answers EFAULT, and the log names the address: a person reads
 * which pointer of which program was wrong instead of a lone number in a
 * shell, and the attack suite of `make mmu-smoke` is a reader of this line.
 */
static bool range_refused(k_u64_t start, k_size_t size)
{
    struct task *self = task_self();

    klog(K_LOG_LEVEL_ERROR,
         "x86-64: the program (pid %u) named 0x%08x%08x (+%u bytes), which is not memory it owns: "
         "EFAULT",
         (self != NULL) ? (unsigned)self->pid : 0u, (unsigned)(start >> 32),
         (unsigned)(start & 0xFFFFFFFFu), (unsigned)size);

    return false;
}

/*
 * The edge of the system call on a machine with page tables. The syscall stub
 * runs in ring 0 but under the CR3 of the program that called it, so a user
 * address of that program is readable by the kernel - once every page of the
 * range is known to be there. The walk below answers that question without a
 * fault: an address outside the window, the null page, the guard page of the
 * stack and the rest of the window that was never mapped all name no page.
 *
 * A kernel task has no such window: its pointers are the memory of the kernel
 * and are accepted as they are, which is the same answer as the host port.
 */
bool k_port_exec_contains(const void *ptr, k_size_t size)
{
    k_u64_t space = port_x86_64_task_space();
    k_u64_t start;
    k_u64_t end;
    k_u64_t at;

    if (space == 0) {
        return true;
    }

    if (size == 0) {
        return true;
    }

    start = (k_u64_t)(uintptr_t)ptr;
    end = start + (k_u64_t)size;

    /* A range that wraps the address space is not a range. */
    if (end < start) {
        return range_refused(start, size);
    }

    if (start < PORT_EXEC_BASE || end > PORT_EXEC_LIMIT) {
        return range_refused(start, size);
    }

    /*
     * The frames of the window are not contiguous, but the page tables are
     * the only thing that can say whether a page is part of the program: a
     * present PTE is the answer, and it covers copy-on-write pages and the
     * text too. A store of the kernel into a page that is read-only - the
     * text of the program, the restorer of a handler - does not stop the
     * machine: port_x86_64_exception() gives the page up as code before the
     * store is retried (port_x86_64_paging_allow_kernel_write()).
     */
    for (at = start & ~(k_u64_t)(PAGE_SIZE - 1u); at < end; at += PAGE_SIZE) {
        if (port_x86_64_paging_phys(space, at) == NULL) {
            return range_refused(start, size);
        }
    }

    return true;
}

bool k_port_exec_active(void)
{
    /* A program is the task that runs: its exit is the exit of the task. */
    return port_x86_64_task_is_user();
}

/*
 * The window of a copy into the memory of a program (ADR 0052). The processor
 * of this port may have SMAP, and then the kernel may not read or write a page
 * of ring 3 until AC is set; the portable kernel opens this around every copy
 * it makes through a pointer of a program. On a processor without the bit the
 * pair does nothing, which is why the kernel can call it unconditionally.
 */
void k_port_exec_user_access_begin(void)
{
    port_x86_64_user_access_begin();
}

void k_port_exec_user_access_end(void)
{
    port_x86_64_user_access_end();
}
void k_port_exec_exit(int code)
{
    task_exit(code);
}

/*
 * The child of a fork on a machine with page tables (K4). The tables of the
 * parent are copied with every page of the window made read-only in both
 * spaces; the frame of the system call that is being served is copied to the
 * top of the child's kernel stack with zero in place of its answer. The child
 * is then a task that returns from the call at the instruction after it, and
 * the parent returns from this one with the pid.
 */
/*
 * The microseconds between two readings of the TSC, for the line of a fork.
 * `k_port_uptime_us()` of the port counts the ticks of the machine and its
 * unit is the millisecond; the fork of ADR 0057 is measured in microseconds
 * and the TSC is the clock of the processor that has them.
 */
static k_u64_t fork_microseconds(k_u64_t ticks)
{
    const struct port_x86_64_info *info = port_x86_64_info();
    k_u64_t hz;

    if (info == NULL || info->cpu_hz == 0u) {
        return ticks;
    }

    hz = info->cpu_hz;

    return (ticks / hz) * 1000000u + ((ticks % hz) * 1000000u) / hz;
}

int k_port_fork_copy(struct task *child)
{
    k_u64_t parent_space = port_x86_64_task_space();
    k_u64_t parent_top = port_x86_64_task_stack_top();
    k_u64_t child_space;
    k_u64_t started;
    k_u64_t cloned;
    k_u32_t shared = 0;
    const void *frame;
    int rc;

    if (child == NULL) {
        return -2;
    }

    /*
     * The caller has to be a program of ring 3: a fork of a kernel task has
     * no user frame to copy, and the kernel keeps its placeholder for it.
     */
    if (parent_space == 0 || parent_top < PORT_X86_64_SYSCALL_FRAME_SIZE) {
        return -1;
    }

    started = port_rdtsc();

    child_space = port_x86_64_paging_space_clone(parent_space, &shared);
    if (child_space == 0) {
        return -2;
    }

    cloned = port_rdtsc();

    frame = (const void *)(uintptr_t)(parent_top - PORT_X86_64_SYSCALL_FRAME_SIZE);
    rc = port_x86_64_task_fork_create(child, child_space, frame, PORT_X86_64_SYSCALL_FRAME_SIZE,
                                      port_exec_fork_child);
    if (rc != 0) {
        port_x86_64_paging_space_destroy(child_space);
        return -2;
    }

    /*
     * The line of the promise of ADR 0057: the call shares the frames of the
     * parent and copies no byte of them, so a program of 64 MiB is the walk
     * of its tables and this number is the cost of the call. The clock is the
     * TSC of the processor, which is the one clock of this machine that has a
     * microsecond in it (`k_port_uptime_us()` counts the ticks of the timer,
     * and its unit is the millisecond). The first number is the whole call,
     * the second the tables of the child alone - the task of the child, its
     * stack and its frame are the rest.
     */
    kinfo("x86-64: fork (pid %u) shares %u page(s) in %u us (tables %u us)", (unsigned)child->pid,
          (unsigned)shared, (unsigned)fork_microseconds(port_rdtsc() - started),
          (unsigned)fork_microseconds(cloned - started));

    return (int)child->pid;
}

/*
 * Place an image in an address space of its own: the pages of the text and
 * the data, the stack, the restorer of a handler and the two vectors of the
 * entry, at the addresses the program will see. The answer is 0 with `start`
 * filled, or -1 after the space this call made has been given back.
 *
 * Both shapes of execve start here: the program that replaces the caller of
 * the call (port_exec_replace) and the program the kernel starts for a caller
 * without an address space (the shell), which is a task of its own.
 */
static int port_exec_place(const struct k_exec_image *image, char *const argv[],
                           struct port_exec_start *start)
{
    k_u64_t space;
    k_u64_t user_rsp;
    k_u64_t user_argc = 0;
    k_u64_t user_argv = 0;
    k_u64_t user_envc = 0;
    k_u64_t user_envp = 0;
    k_size_t source;
    k_size_t end;
    k_size_t text_end;
    k_size_t rw_end;
    k_size_t arg_off = 0;
    k_size_t arg_end = 0;
    k_size_t strings = 0;
    k_size_t array_off = 0;
    k_size_t env_off = 0;
    k_size_t env_end = 0;
    k_size_t env_strings = 0;
    k_size_t env_array_off = 0;
    k_size_t vectors_end = 0;
    k_size_t index;

    if (image == NULL || image->text_size == 0) {
        return -1;
    }

    /* The bytes are in the memory of the caller or in the file the port
     * maps; neither is an image. */
    if (image->data == NULL && image->path == NULL) {
        return -1;
    }

    /*
     * The image has to be whole: the header, the text and the data are inside
     * the bytes the caller holds. The two comparisons after the sum catch a
     * wrap of the counter, which is how a header that lies about its sizes
     * gets past a single check.
     */
    source = (k_size_t)K_EXEC_HEADER_SIZE + image->text_size + image->data_size;
    if (source < image->text_size || source < image->data_size || source > image->size) {
        return -1;
    }

    if (image->entry < K_EXEC_HEADER_SIZE ||
        image->entry >= (k_size_t)K_EXEC_HEADER_SIZE + image->text_size) {
        return -1;
    }

    if (image->bss_size > K_EXEC_BSS_MAX) {
        return -1;
    }

    end = source + image->bss_size;

    /*
     * A mapped image and its vectors are page aligned: the heap of the
     * program begins on the page after the image, and the vectors of the
     * entry are the first thing in it.
     */
    if (image->data == NULL) {
        end = align_up(end, PAGE_SIZE);
    }

    if (!image_fits(image, source, end)) {
        kerror("x86_64: the image (%u bytes) does not fit the window of the programs",
               (unsigned)end);
        return -1;
    }

    /*
     * The text ends on a page of its own. A program is not relocated, so the
     * pages of the format are the pages of the map: the text is executable
     * and read-only, the data that follows it is writable and NX, and one
     * page cannot be both. program.ld pads the text to the boundary and the
     * port refuses an image that does not (ADR 0007, NX and W^X).
     */
    text_end = (k_size_t)K_EXEC_HEADER_SIZE + image->text_size;
    if ((text_end % PAGE_SIZE) != 0) {
        kerror("x86_64: the text of the image does not end on a page (%u bytes)",
               (unsigned)text_end);
        return -1;
    }

    /*
     * The arguments are copied too: the vector of the kernel is not memory a
     * program of ring 3 may read. The strings go after the image, the array
     * of pointers after them, and the array carries the virtual addresses the
     * program will see.
     */
    if (argv != NULL) {
        while (user_argc < PORT_EXEC_ARGC_MAX && argv[user_argc] != NULL) {
            k_size_t length = (k_size_t)strlen(argv[user_argc]);

            if (strings + length + 1u > PORT_EXEC_ARG_BYTES_MAX) {
                return -1;
            }

            strings += length + 1u;
            user_argc++;
        }
    }

    /*
     * The environment is a vector of the same shape and is copied the same
     * way, after the arguments. A vector that is there and empty is a program
     * with no environment, which is not the same as no vector: the entry then
     * receives a pointer to a list whose first word is NULL.
     */
    if (image->envp != NULL) {
        while (user_envc < PORT_EXEC_ENVP_MAX && image->envp[user_envc] != NULL) {
            k_size_t length = (k_size_t)strlen(image->envp[user_envc]);

            if (env_strings + length + 1u > PORT_EXEC_ENV_BYTES_MAX) {
                return -1;
            }

            env_strings += length + 1u;
            user_envc++;
        }
    }

    if (user_argc > 0) {
        arg_off = align_up(end, 16u);
        array_off = align_up(arg_off + strings, 8u);
        arg_end = array_off + (k_size_t)(user_argc + 1u) * sizeof(k_u64_t);
    }

    if (image->envp != NULL) {
        env_off = align_up((user_argc > 0) ? arg_end : end, 8u);
        env_array_off = align_up(env_off + env_strings, 8u);
        env_end = env_array_off + (k_size_t)(user_envc + 1u) * sizeof(k_u64_t);
    }

    /*
     * Both vectors live below the restorer of a handler, the page the port
     * keeps under the guard page of the stack; the one that ends last
     * decides. A zero vector occupies nothing and passes.
     */
    vectors_end = (env_end > arg_end) ? env_end : arg_end;

    if (vectors_end > (k_size_t)PORT_EXEC_RESTORER_OFFSET) {
        kerror("x86_64: the arguments or the environment do not fit below the stack");
        return -1;
    }

    space = port_x86_64_paging_space_create();
    if (space == 0) {
        kerror("x86_64: no page tables for a program");
        return -1;
    }

    /*
     * The stack and the restorer of a handler are made now, because the
     * kernel writes them and the program may reach them before any of its
     * own pages. The rest of the window is left out on purpose, so a program
     * that runs past its data or below its stack meets a page that is not
     * there instead of a page of another program.
     */
    if (!space_map(space, PORT_EXEC_STACK_OFFSET, PORT_EXEC_STACK_SIZE, PORT_X86_64_PAGE_WRITE)) {
        goto fail;
    }

    /* The restorer of a handler is a page of code of the program, written by
     * the kernel before the entry: it is executable and never writable. */
    if (!space_map(space, PORT_EXEC_RESTORER_OFFSET, PAGE_SIZE, PORT_X86_64_PAGE_EXEC)) {
        goto fail;
    }

    if (image->data == NULL) {
        /*
         * The image is a file and not a block of the kernel: the regions of
         * the text, the data and the bss are recorded and the pages come from
         * the file when the program touches them (kernel/mm.h). A program of
         * 4 MiB costs the pages it runs, and no second copy of it exists.
         */
        if (k_mm_image(space, image) != 0) {
            kerror("x86_64: the image %s cannot be mapped", image->path);
            goto fail;
        }

        if (vectors_end > end) {
            if (k_mm_brk(space, PORT_EXEC_BASE + (k_u64_t)align_up(vectors_end, PAGE_SIZE), NULL) !=
                0) {
                goto fail;
            }

            if (k_mm_touch(space, PORT_EXEC_BASE + (k_u64_t)end, vectors_end - (k_size_t)end,
                           true) != 0) {
                kerror("x86_64: the vectors of the entry cannot be written");
                goto fail;
            }
        }
    } else {
        /*
         * The machine has the bytes in the kernel: the image is mapped page
         * by page. The text - the header and the code - is the one executable
         * region of a program and it is not writable. Everything after it,
         * the initialised data, the bss, the vector of the arguments and the
         * environment, is writable and NX: the program may compute into it,
         * never run it.
         */
        if (!space_map(space, 0, text_end, PORT_X86_64_PAGE_EXEC)) {
            goto fail;
        }

        rw_end = align_up(end, PAGE_SIZE);

        if (vectors_end > rw_end) {
            rw_end = align_up(vectors_end, PAGE_SIZE);
        }

        if (rw_end > text_end &&
            !space_map(space, text_end, rw_end - text_end, PORT_X86_64_PAGE_WRITE)) {
            goto fail;
        }

        /*
         * The bytes live in the frames of the arena; the program sees them
         * through its page tables at the base of the window. The header is
         * copied with the segments because the entry of the image is an offset
         * from its start.
         */
        if (!space_write(space, PORT_EXEC_BASE, image->data, source)) {
            goto fail;
        }

        if (image->bss_size != 0 &&
            !space_zero(space, PORT_EXEC_BASE + (k_u64_t)source, image->bss_size)) {
            goto fail;
        }
    }

    /* The tables have to keep the promise before a byte of the program runs:
     * a page that is writable and executable at once is a refusal here and
     * not a fault on the machine. The line is the report of the audit that
     * every `exec` leaves in the log (ADR 0048). */
    if (port_x86_64_paging_audit(space) != 0) {
        kerror("x86_64: the map of the program is not W^X");
        goto fail;
    }

    /*
     * The report of the audit at every exec (ADR 0048). It is one short
     * line on purpose: the console of the machine is a UART and a line is
     * written byte by byte with the interrupts off, and a program of the
     * desktop is entered right after this one - the log must not hold the
     * entry of a window for the milliseconds of a longer sentence.
     */
    kinfo("x86-64: the map of a program is W^X");

    if (!space_write(space, PORT_EXEC_RESTORER, g_restorer_code, sizeof(g_restorer_code))) {
        goto fail;
    }

    if (user_argc > 0) {
        k_size_t cursor = arg_off;

        for (index = 0; index < (k_size_t)user_argc; index++) {
            k_size_t length = (k_size_t)strlen(argv[index]) + 1u;
            k_u64_t pointer = PORT_EXEC_BASE + (k_u64_t)cursor;

            if (!space_write(space, pointer, argv[index], length) ||
                !space_write(space,
                             PORT_EXEC_BASE + (k_u64_t)array_off + (k_u64_t)index * sizeof(k_u64_t),
                             &pointer, sizeof(pointer))) {
                goto fail;
            }

            cursor += length;
        }

        {
            k_u64_t terminator = 0;

            if (!space_write(space,
                             PORT_EXEC_BASE + (k_u64_t)array_off +
                                 (k_u64_t)user_argc * sizeof(k_u64_t),
                             &terminator, sizeof(terminator))) {
                goto fail;
            }
        }

        user_argv = PORT_EXEC_BASE + (k_u64_t)array_off;
    }

    if (image->envp != NULL) {
        k_size_t cursor = env_off;

        for (index = 0; index < (k_size_t)user_envc; index++) {
            k_size_t length = (k_size_t)strlen(image->envp[index]) + 1u;
            k_u64_t pointer = PORT_EXEC_BASE + (k_u64_t)cursor;

            if (!space_write(space, pointer, image->envp[index], length) ||
                !space_write(space,
                             PORT_EXEC_BASE + (k_u64_t)env_array_off +
                                 (k_u64_t)index * sizeof(k_u64_t),
                             &pointer, sizeof(pointer))) {
                goto fail;
            }

            cursor += length;
        }

        {
            k_u64_t terminator = 0;

            if (!space_write(space,
                             PORT_EXEC_BASE + (k_u64_t)env_array_off +
                                 (k_u64_t)user_envc * sizeof(k_u64_t),
                             &terminator, sizeof(terminator))) {
                goto fail;
            }
        }

        user_envp = PORT_EXEC_BASE + (k_u64_t)env_array_off;
    }

    /*
     * The stack of the program starts as zeros, and the top word is the zero
     * a `ret` would pop. It belongs to this program and not to the frame
     * allocator's previous owner: every page was cleared when it was handed
     * out.
     */
    user_rsp = PORT_EXEC_BASE + PORT_EXEC_STACK_OFFSET + PORT_EXEC_STACK_SIZE - sizeof(k_u64_t);
    {
        k_u64_t zero = 0;

        if (!space_write(space, user_rsp, &zero, sizeof(zero))) {
            goto fail;
        }
    }

    start->space = space;
    start->entry = PORT_EXEC_BASE + (k_u64_t)image->entry;
    start->argc = user_argc;
    start->argv = user_argv;
    start->envp = user_envp;
    start->user_rsp = user_rsp;

    return 0;

fail:
    port_x86_64_paging_space_destroy(space);
    return -1;
}

/*
 * The POSIX shape of execve for a program of ring 3: the image replaces the
 * memory of the caller and the call does not return to the old program. The
 * caller is the program itself - the same task, its pid, its parent, its
 * descriptors and its directory - and only the address space is new.
 *
 * The return is the one the program would have made anyway: the stub of
 * `int $0x80` below this call pops the frame it pushed and leaves through
 * `iretq`. Rewriting that frame is the whole of the transfer: RIP becomes the
 * entry, RSP the new stack, RDI, RSI and RDX the three arguments of the C
 * entry (ADR 0016), and the other registers zero, so the new program starts
 * with no state of the old one anywhere. The frame is at the top of the
 * kernel stack, above every C frame of this call, which the iretq abandons.
 *
 * CR3 is switched before the return, so the iretq and the first instruction
 * of the program see the new tables. The kernel is mapped in every space
 * (paging.c), so the few instructions that return through the stub are as
 * valid under the new CR3 as under the old one. The old space is given back
 * afterwards: its frames are released by the reference count, and a page a
 * child of the caller shares is still the child's.
 */
static int port_exec_replace(const struct port_exec_start *start)
{
    k_u64_t top = port_x86_64_task_stack_top();
    k_u64_t previous = port_x86_64_task_space();
    k_u64_t *frame;

    if (previous == 0 || top < PORT_X86_64_SYSCALL_FRAME_SIZE) {
        port_x86_64_paging_space_destroy(start->space);
        return K_EXEC_RUN_REFUSED;
    }

    frame = (k_u64_t *)(uintptr_t)(top - PORT_X86_64_SYSCALL_FRAME_SIZE);

    /*
     * The handlers of the old image are code that is about to be gone: POSIX
     * resets a caught signal to its default action across an exec and keeps
     * an ignored one ignored. The pending marks stay, as POSIX leaves them.
     */
    task_signal_reset_on_exec(task_self());

    /*
     * Every word of the frame is a register the stub restores: the entry of
     * the program reads its three arguments from RDI, RSI and RDX, and the
     * rest are zero so that no value of the replaced program survives. RAX
     * is zero too - the answer of a successful execve, which the new program
     * never reads, but a person looking at a register dump does.
     */
    memset(frame, 0, PORT_X86_64_SYSCALL_FRAME_SIZE);

    frame[PORT_FRAME_RDI] = start->argc;
    frame[PORT_FRAME_RSI] = start->argv;
    frame[PORT_FRAME_RDX] = start->envp;
    frame[PORT_FRAME_RIP] = start->entry;
    frame[PORT_FRAME_CS] = 0x1b;      /* the ring 3 code selector of gdt.c */
    frame[PORT_FRAME_RFLAGS] = 0x202; /* interrupts on, the required bit 1 */
    frame[PORT_FRAME_RSP] = start->user_rsp;
    frame[PORT_FRAME_SS] = 0x23; /* the ring 3 data selector of gdt.c */

    /*
     * The new image starts with the floating point unit after fninit: the
     * numbers of the program that called execve are not part of the image it
     * asked for, as POSIX leaves them. The reset is on the live unit and on
     * the saved image of the task, so a switch before the iretq cannot bring
     * the old registers back (docs/debts.md, K9).
     */
    port_x86_64_task_fpu_reset();

    port_x86_64_task_set_space(start->space);
    port_x86_64_paging_space_destroy(previous);

    return K_EXEC_RUN_REPLACED;
}

int k_port_exec_enter(const struct k_exec_image *image, char *const argv[])
{
    struct task *caller;
    struct task *program;
    struct port_exec_start *start;
    struct port_exec_start placed;
    int status = 0;
    int rc;

    caller = task_self();
    if (caller == NULL) {
        /* The caller has to be a task: execve waits for the program to end,
         * and a wait is a sleep, which the boot context cannot do. */
        kerror("x86_64: execve outside of a task");
        return -1;
    }

    rc = port_exec_place(image, argv, &placed);
    if (rc != 0) {
        return -1;
    }

    /*
     * A caller with an address space is a program, and its execve replaces
     * the image it runs: POSIX, and the answer of the call is not a return
     * to the old code at all. A caller without one is a task of the kernel -
     * the shell - and a program it starts is a task of its own that the
     * caller waits for; there is nothing to replace on a task with no
     * memory of a program.
     */
    if (port_x86_64_task_space() != 0) {
        return port_exec_replace(&placed);
    }

    /* The program inherits the descriptors of its caller, as POSIX leaves
     * them across an exec: the table is copied before the task can run, so a
     * shell that made a pipe and a redirect finds them in the program at 0,
     * 1 and 2. The same copy carries the working directory and the job. */
    start = (struct port_exec_start *)k_port_alloc(sizeof(*start), false);
    if (start == NULL) {
        kerror("x86_64: no memory for the start of a program");
        port_x86_64_paging_space_destroy(placed.space);
        return -1;
    }

    *start = placed;

    program = task_fork_shared(caller, PORT_EXEC_TASK_NAME, port_exec_program_task, start);
    if (program == NULL) {
        k_port_free(start);
        port_x86_64_paging_space_destroy(placed.space);
        return -1;
    }

    /*
     * The caller of execve waits for the program the way a parent waits for
     * a child: the program is a task of the scheduler, not a call that stays
     * on this stack. A program that returns a status goes through exit, and
     * a program that faults is taken down with 255 by the fault handler.
     *
     * The foreground program gets its own process group and the terminal
     * foreground, so Ctrl+C reaches the program and not the shell or init.
     * The previous foreground group is restored when the program ends.
     *
     * A task that belongs to a job is a stage of a pipeline: the owner of
     * the job (the shell) reads the console and signals every stage at once,
     * so this wait must not take the key for itself.
     */
    const bool own_console = (task_get_job(caller) == TASK_JOB_NONE);
    k_u32_t old_fg = 0;

    if (own_console) {
        old_fg = k_tty_fg_pgrp();
        (void)task_setpgid(program->pid, program->pid);
        (void)k_tty_set_fg_pgrp(program->pid);
    }

    {
        const k_u32_t step_ms = 5u;
        k_u32_t waited = 0;

        for (;;) {
            /*
             * The console is watched for Ctrl+C only while no program reads
             * it: a program that reads its terminal owns every key, and the
             * Ctrl+C of an editor in raw mode is the byte it asked for
             * (ADR 0051 §8).
             */
            if (own_console && !k_tty_console_claimed()) {
                int key = k_console_poll();

                if (key == 3) {
                    (void)task_signal_send(program->pid, K_SIGINT);
                }
            }

            rc = task_wait(program->pid, &status, step_ms);
            if (rc != -2) {
                break;
            }

            waited += step_ms;
            if (waited >= PORT_EXEC_WAIT_MS) {
                /*
                 * The program is older than the wait. It is told to stop
                 * the way Ctrl+C tells it and given the second the step
                 * takes: the caller that gives up must not leave a program
                 * writing the volume behind it, or the next line of the
                 * shell is a second writer of the same files.
                 */
                (void)task_signal_send(program->pid, K_SIGINT);
                rc = task_wait(program->pid, &status, PORT_EXEC_STOP_MS);
                break;
            }
        }
    }

    if (rc == -2) {
        /*
         * The budget of the call is spent. The task is a task of the
         * scheduler: it does not end because the caller stopped waiting, and
         * returning here would leave it running with the console of the
         * shell and a slot that nobody reaps - which is what the session of
         * the release caught as two tasks named `program` in a table of
         * four. It is stopped, given a moment to become a zombie and reaped.
         */
        int tries;

        (void)task_signal_send(program->pid, K_SIGTERM);
        for (tries = 0; tries < (int)(PORT_EXEC_STOP_MS / 5u); tries++) {
            if (task_wait(program->pid, &status, 5) != -2) {
                break;
            }
        }
        kerror("x86_64: the program ran past %u ms and was stopped", PORT_EXEC_WAIT_MS);
        if (own_console) {
            (void)k_tty_set_fg_pgrp(old_fg != 0u ? old_fg : task_get_pgid(caller));
        }
        return -1;
    }

    if (rc != 0) {
        kerror("x86_64: the program did not end (%d)", rc);
        if (own_console) {
            (void)k_tty_set_fg_pgrp(old_fg != 0u ? old_fg : task_get_pgid(caller));
        }
        return -1;
    }

    k_exec_exit(status);
    kinfo("x86-64: the program exited with %d", status & 0xFF);

    if (own_console) {
        (void)k_tty_set_fg_pgrp(old_fg != 0u ? old_fg : task_get_pgid(caller));
    }
    return 0;
}

/* --- signals --- */

/*
 * The frame of the running task, when the caller is a program of ring 3. A
 * stub of the port leaves it at the top of the kernel stack of the program:
 * the fifteen registers it saved and the five of the crossing, 160 bytes.
 * Every entry into the kernel from user space has that shape, so the address
 * is the same for a system call and for the tick of the timer.
 */
static k_u64_t *user_frame(void)
{
    k_u64_t top = port_x86_64_task_stack_top();

    if (!port_x86_64_task_is_user() || top < PORT_X86_64_SYSCALL_FRAME_SIZE) {
        return NULL;
    }

    return (k_u64_t *)(uintptr_t)(top - PORT_X86_64_SYSCALL_FRAME_SIZE);
}

/*
 * Save the registers of the frame into the signal frame. The order of the
 * two stubs differs (see PORT_FRAME_* and interrupts.S), and `from_syscall`
 * says which one made the frame.
 */
static void frame_to_signal(const k_u64_t *frame, bool from_syscall, struct port_signal_frame *out)
{
    memset(out, 0, sizeof(*out));

    out->magic = PORT_SIGNAL_MAGIC;
    out->rbp = frame[PORT_FRAME_RBP];
    out->rax = frame[PORT_FRAME_RAX];
    out->rip = frame[PORT_FRAME_RIP];
    out->cs = frame[PORT_FRAME_CS];
    out->rflags = frame[PORT_FRAME_RFLAGS];
    out->rsp = frame[PORT_FRAME_RSP];
    out->ss = frame[PORT_FRAME_SS];
    out->r9 = frame[3];
    out->r8 = frame[4];

    if (from_syscall) {
        out->rcx = frame[1];
        out->r11 = frame[2];
        out->r10 = frame[5];
        out->rdx = frame[6];
        out->rsi = frame[7];
        out->rdi = frame[8];
    } else {
        out->r11 = frame[1];
        out->r10 = frame[2];
        out->rdi = frame[5];
        out->rsi = frame[6];
        out->rdx = frame[7];
        out->rcx = frame[8];
    }
}

/* The reverse of frame_to_signal into the frame of the system call that is
 * being served: its stub pops the registers in the order of the call. */
static void signal_to_frame(const struct port_signal_frame *in, k_u64_t *frame)
{
    frame[PORT_FRAME_RBP] = in->rbp;
    frame[PORT_FRAME_RAX] = in->rax;
    frame[PORT_FRAME_RIP] = in->rip;
    frame[PORT_FRAME_RSP] = in->rsp;

    /*
     * The selectors and the flags stay those of the sigreturn call itself,
     * which the processor built when the handler's restorer entered ring 0:
     * a frame on the stack of the program is memory the program can write,
     * and an iretq that trusted a selector from it would be a door into
     * ring 0 (and an IOPL from it a door to the ports of the machine). The
     * saved copies are still in the frame for a dump; they are not restored.
     */
    frame[3] = in->r9;
    frame[4] = in->r8;
    frame[1] = in->rcx;
    frame[2] = in->r11;
    frame[5] = in->r10;
    frame[6] = in->rdx;
    frame[7] = in->rsi;
    frame[8] = in->rdi;
}

int k_port_task_signal_enter(struct task *t, int sig, void (*handler)(int), k_ssize_t result,
                             bool from_syscall)
{
    struct port_signal_frame saved;
    k_u64_t *frame = user_frame();
    k_u64_t frame_addr;
    k_u64_t old_rsp;
    k_u64_t restorer = PORT_EXEC_RESTORER;

    K_UNUSED(t);

    if (frame == NULL) {
        /* A task of the kernel: the kernel runs the handler itself. */
        return 0;
    }

    /* Only the frame of a program of ring 3 has a handler to enter. */
    if ((frame[PORT_FRAME_CS] & 3u) != 3u) {
        return 0;
    }

    old_rsp = frame[PORT_FRAME_RSP];

    /*
     * The frame goes below the stack pointer of the program, aligned so that
     * the handler starts with the alignment of a function of the ABI: the
     * return address is one word below the aligned frame.
     */
    frame_addr = (old_rsp - (k_u64_t)sizeof(saved)) & ~(k_u64_t)15u;

    /*
     * The frame is written below the stack pointer, and a program in the
     * middle of a frame of its own - or one that has just moved RSP into the
     * room of ADR 0052 - may have no page there yet. The port grows the stack
     * for the frame it is about to write, the same way it grows it for a
     * store of the program; an address outside the room is still refused and
     * the signal still ends the task.
     */
    (void)port_x86_64_stack_grow(frame_addr - sizeof(k_u64_t));

    if (!k_port_exec_contains((const void *)(uintptr_t)(frame_addr - sizeof(k_u64_t)),
                              sizeof(saved) + sizeof(k_u64_t))) {
        /* The stack of the program cannot carry the frame: the signal is the
         * end of the task, as it is for any other fault of a handler. */
        return -1;
    }

    frame_to_signal(frame, from_syscall, &saved);

    /*
     * The interrupted call has an answer, and the program sees it when the
     * handler returns: for a wait that a signal cut, it is EINTR. A signal
     * that arrived from a device keeps the register the program had.
     */
    if (from_syscall) {
        saved.rax = (k_u64_t)result;
    }

    /*
     * The kernel is on the page tables of the program here (a call of ring 3
     * and an interrupt of ring 3 both keep them), and k_port_exec_contains()
     * has said that every page of the range is there. The window around the
     * two copies is what SMAP asks for (ADR 0052): a supervisor store into a
     * page of ring 3 is refused unless AC is set.
     */
    port_x86_64_user_access_begin();
    memcpy((void *)(uintptr_t)frame_addr, &saved, sizeof(saved));
    memcpy((void *)(uintptr_t)(frame_addr - sizeof(k_u64_t)), &restorer, sizeof(restorer));
    port_x86_64_user_access_end();

    /*
     * The handler is a function of one argument: the number of the signal.
     * The place of RDI is not the same in the frame of a call and in the
     * frame of an interrupt (see frame_to_signal).
     */
    frame[from_syscall ? 8u : 5u] = (k_u64_t)sig;
    frame[PORT_FRAME_RIP] = (k_u64_t)(uintptr_t)handler;
    frame[PORT_FRAME_RSP] = frame_addr - sizeof(k_u64_t);

    return 1;
}

int k_port_task_signal_leave(struct task *t, k_ssize_t *result)
{
    struct port_signal_frame saved;
    k_u64_t *frame = user_frame();

    K_UNUSED(t);

    if (frame == NULL || (frame[PORT_FRAME_CS] & 3u) != 3u) {
        return KERR_NOSYS;
    }

    if (!k_port_exec_contains((const void *)(uintptr_t)frame[PORT_FRAME_RSP], sizeof(saved))) {
        return KERR_INVAL;
    }

    port_x86_64_user_access_begin();
    memcpy(&saved, (const void *)(uintptr_t)frame[PORT_FRAME_RSP], sizeof(saved));
    port_x86_64_user_access_end();

    if (saved.magic != PORT_SIGNAL_MAGIC) {
        return KERR_INVAL;
    }

    signal_to_frame(&saved, frame);

    /* The stub of the call writes this value into the place of the result;
     * it is the value the interrupted context had, not the number of the
     * sigreturn call. A negative answer of the interrupted call is a value
     * and not the failure of this call. */
    if (result != NULL) {
        *result = (k_ssize_t)saved.rax;
    }

    return 0;
}

/*
 * The tick of the timer is a point where a program comes into the kernel, so
 * it is also a point where a pending signal can be answered. The frame of the
 * interrupt is the frame of the program; the handler is entered when the
 * iretq of the stub goes back to ring 3.
 */
void port_x86_64_signal_tick(k_u64_t *frame)
{
    struct task *self;

    if (frame == NULL || (frame[PORT_FRAME_CS] & 3u) != 3u) {
        return;
    }

    self = task_self();
    if (self == NULL) {
        return;
    }

    task_signal_check(self, 0, false);
}

void port_x86_64_program_fault(k_u64_t vector, k_u64_t error, k_u64_t address, k_u64_t rip)
{
    struct task *self = task_self();

    /*
     * The error code is part of the line because it names the access: the
     * program that fetched an NX page and the one that wrote its own text
     * both meet vector 14, and bit 4 of the code is the only thing that tells
     * the two apart. The smoke test of the port reads it.
     */
    klog(K_LOG_LEVEL_ERROR,
         "the program faulted: vector %u at 0x%08x%08x, error 0x%08x%08x, rip 0x%08x%08x",
         (unsigned)vector, (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu),
         (unsigned)(error >> 32), (unsigned)(error & 0xFFFFFFFFu), (unsigned)(rip >> 32),
         (unsigned)(rip & 0xFFFFFFFFu));

    /*
     * The reason, in the words of the ABI (ADR 0052). A page fault is a
     * program that touched an address it does not own - the page under its
     * stack, a page of the kernel, the text it may not write, the stack it
     * may not run - and the answer of a bad address is EFAULT. The line
     * carries the address twice on purpose: the first line has the number CR2
     * holds, the second the sentence a person reads. Any other vector is not
     * an address and does not wear its name: the instruction is named and the
     * task ends with the same status.
     */
    if (vector == 14u) {
        klog(K_LOG_LEVEL_ERROR,
             "x86-64: the program (pid %u) touched 0x%08x%08x and that is not a page it may use: "
             "EFAULT, the task ends, the machine stays",
             (self != NULL) ? (unsigned)self->pid : 0u, (unsigned)(address >> 32),
             (unsigned)(address & 0xFFFFFFFFu));
    } else {
        klog(K_LOG_LEVEL_ERROR,
             "x86-64: the program (pid %u) met exception %u at 0x%08x%08x: the task ends, the "
             "machine stays",
             (self != NULL) ? (unsigned)self->pid : 0u, (unsigned)vector, (unsigned)(address >> 32),
             (unsigned)(address & 0xFFFFFFFFu));
    }

    /* A fault of a program is the end of its task, not of the machine. */
    task_exit(255);

    /* Not reached: the exit of a task does not come back. */
    for (;;) {
    }
}
