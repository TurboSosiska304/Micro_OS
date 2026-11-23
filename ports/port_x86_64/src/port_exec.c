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
#include "kernel/errno.h"
#include "kernel/ksys.h"
#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port/exec.h"
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
#define PORT_EXEC_STACK_SIZE (64u * 1024u)
#define PORT_EXEC_STACK_OFFSET (PORT_EXEC_WINDOW - PORT_EXEC_STACK_SIZE)
#define PORT_EXEC_GUARD_OFFSET (PORT_EXEC_STACK_OFFSET - PAGE_SIZE)

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
#define PORT_EXEC_RESTORER_OFFSET (PORT_EXEC_GUARD_OFFSET - PAGE_SIZE)
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

/* How long the caller of execve waits for the program it started. */
/*
 * How long execve waits for the program before it gives up. The wait is the
 * life of the call and not a timeout of the machine: a program of this
 * system reads a file, writes a screen and computes, and a minute is five
 * times the slowest one of the release on a loaded machine. A program that
 * is still there at the end is stopped and reaped - a call that returned
 * cannot leave a task behind with a console of its own - so the number is
 * the point of the stop and not a leak.
 */
#define PORT_EXEC_WAIT_MS 60000u

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
        return false;
    }

    if (start < PORT_EXEC_BASE || end > PORT_EXEC_LIMIT) {
        return false;
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
            return false;
        }
    }

    return true;
}

bool k_port_exec_active(void)
{
    /* A program is the task that runs: its exit is the exit of the task. */
    return port_x86_64_task_is_user();
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
int k_port_fork_copy(struct task *child)
{
    k_u64_t parent_space = port_x86_64_task_space();
    k_u64_t parent_top = port_x86_64_task_stack_top();
    k_u64_t child_space;
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

    child_space = port_x86_64_paging_space_clone(parent_space);
    if (child_space == 0) {
        return -2;
    }

    frame = (const void *)(uintptr_t)(parent_top - PORT_X86_64_SYSCALL_FRAME_SIZE);
    rc = port_x86_64_task_fork_create(child, child_space, frame, PORT_X86_64_SYSCALL_FRAME_SIZE,
                                      port_exec_fork_child);
    if (rc != 0) {
        port_x86_64_paging_space_destroy(child_space);
        return -2;
    }

    return (int)child->pid;
}

int k_port_exec_enter(const struct k_exec_image *image, char *const argv[])
{
    struct task *caller;
    struct task *program;
    struct port_exec_start *start;
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
    k_size_t index;
    int status = 0;
    int rc;

    caller = task_self();
    if (caller == NULL) {
        /* The caller has to be a task: execve waits for the program to end,
         * and a wait is a sleep, which the boot context cannot do. */
        kerror("x86_64: execve outside of a task");
        return -1;
    }

    if (image == NULL || image->data == NULL || image->text_size == 0) {
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
    if (arg_end > (k_size_t)PORT_EXEC_RESTORER_OFFSET ||
        env_end > (k_size_t)PORT_EXEC_RESTORER_OFFSET) {
        kerror("x86_64: the arguments or the environment do not fit below the stack");
        return -1;
    }

    space = port_x86_64_paging_space_create();
    if (space == 0) {
        kerror("x86_64: no page tables for a program");
        return -1;
    }

    /*
     * The image is mapped page by page; the rest of the window is left out on
     * purpose, so a program that runs past its data or below its stack meets
     * a page that is not there instead of a page of another program.
     *
     * The text - the header and the code - is the one executable region of a
     * program and it is not writable. Everything after it, the initialised
     * data, the bss, the vector of the arguments and the environment, is
     * writable and NX: the program may compute into it, never run it.
     */
    if (!space_map(space, 0, text_end, PORT_X86_64_PAGE_EXEC)) {
        goto fail;
    }

    rw_end = align_up(end, PAGE_SIZE);

    if (user_argc > 0 || image->envp != NULL) {
        k_size_t vectors_end = (env_end > arg_end) ? env_end : arg_end;
        k_size_t vectors_page = align_up(vectors_end, PAGE_SIZE);

        if (vectors_page > rw_end) {
            rw_end = vectors_page;
        }
    }

    if (rw_end > text_end &&
        !space_map(space, text_end, rw_end - text_end, PORT_X86_64_PAGE_WRITE)) {
        goto fail;
    }

    if (!space_map(space, PORT_EXEC_STACK_OFFSET, PORT_EXEC_STACK_SIZE, PORT_X86_64_PAGE_WRITE)) {
        goto fail;
    }

    /* The restorer of a handler is a page of code of the program, written by
     * the kernel before the entry: it is executable and never writable. */
    if (!space_map(space, PORT_EXEC_RESTORER_OFFSET, PAGE_SIZE, PORT_X86_64_PAGE_EXEC)) {
        goto fail;
    }

    /* The tables have to keep the promise before a byte of the program runs:
     * a page that is writable and executable at once is a refusal here and
     * not a fault on the machine. */
    if (port_x86_64_paging_audit(space) != 0) {
        kerror("x86_64: the map of the program is not W^X");
        goto fail;
    }

    /*
     * The bytes live in the frames of the arena; the program sees them
     * through its page tables at the base of the window. The header is copied
     * with the segments because the entry of the image is an offset from its
     * start.
     */
    if (!space_write(space, PORT_EXEC_BASE, image->data, source)) {
        goto fail;
    }

    if (image->bss_size != 0 &&
        !space_zero(space, PORT_EXEC_BASE + (k_u64_t)source, image->bss_size)) {
        goto fail;
    }

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

    start = (struct port_exec_start *)k_port_alloc(sizeof(*start), false);
    if (start == NULL) {
        kerror("x86_64: no memory for the start of a program");
        goto fail;
    }

    start->space = space;
    start->entry = PORT_EXEC_BASE + (k_u64_t)image->entry;
    start->argc = user_argc;
    start->argv = user_argv;
    start->envp = user_envp;
    start->user_rsp = user_rsp;

    /*
     * The program inherits the descriptors of its caller, as POSIX leaves
     * them across an exec: the table is copied before the task can run, so a
     * shell that made a pipe and a redirect finds them in the program at 0,
     * 1 and 2. The same copy carries the working directory and the job.
     */
    program = task_fork_shared(caller, PORT_EXEC_TASK_NAME, port_exec_program_task, start);
    if (program == NULL) {
        k_port_free(start);
        goto fail;
    }

    /*
     * The caller of execve waits for the program the way a parent waits for
     * a child: the program is a task of the scheduler, not a call that stays
     * on this stack. A program that returns a status goes through exit, and
     * a program that faults is taken down with 255 by the fault handler.
     *
     * The wait is in small steps because the machine has no job control and
     * no group of a terminal: Ctrl+C on the console of the foreground
     * program is the one signal the port can hand it, and the console is
     * read here, by the task that started the program. A key that is not
     * Ctrl+C is dropped: a program that runs does not read a line, and a
     * typeahead nobody asked for is not part of this contract.
     *
     * A task that belongs to a job is a stage of a pipeline: the owner of
     * the job (the shell) reads the console and signals every stage at once,
     * so this wait must not take the key for itself.
     */
    {
        const k_u32_t step_ms = 5u;
        k_u32_t waited = 0;
        const bool own_console = (task_get_job(caller) == TASK_JOB_NONE);

        for (;;) {
            if (own_console) {
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

        (void)task_signal_send(program->pid, K_SIGKILL);
        for (tries = 0; tries < 400; tries++) {
            if (task_wait(program->pid, &status, 5) != -2) {
                break;
            }
        }
        kerror("x86_64: the program ran past %u ms and was stopped", PORT_EXEC_WAIT_MS);
        return -1;
    }

    if (rc != 0) {
        kerror("x86_64: the program did not end (%d)", rc);
        return -1;
    }

    k_exec_exit(status);
    kinfo("x86-64: the program exited with %d", status & 0xFF);

    return 0;

fail:
    port_x86_64_paging_space_destroy(space);
    return -1;
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
     * has said that every page of the range is there.
     */
    memcpy((void *)(uintptr_t)frame_addr, &saved, sizeof(saved));
    memcpy((void *)(uintptr_t)(frame_addr - sizeof(k_u64_t)), &restorer, sizeof(restorer));

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

    memcpy(&saved, (const void *)(uintptr_t)frame[PORT_FRAME_RSP], sizeof(saved));

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

    /* A fault of a program is the end of its task, not of the machine. */
    task_exit(255);

    /* Not reached: the exit of a task does not come back. */
    for (;;) {
    }
}
