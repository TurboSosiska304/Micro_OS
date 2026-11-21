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

#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port/exec.h"
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
#define PORT_EXEC_WAIT_MS 10000u

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
         * pointer of ring 3, the ten saved registers, the iretq.
         */
        ".globl port_exec_fork_return\n"
        ".type port_exec_fork_return, @function\n"
        "port_exec_fork_return:\n"
        "    movq %rdi, %r12\n"
        "    andq $-16, %rsp\n"
        "    call port_x86_64_tls_user\n"
        "    movq %r12, %rsp\n"
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
        "    iretq\n"
        ".size port_exec_fork_return, . - port_exec_fork_return\n");

static k_size_t align_up(k_size_t value, k_size_t alignment)
{
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static k_size_t align_down(k_size_t value, k_size_t alignment)
{
    return value & ~(alignment - 1u);
}

static bool image_fits(const struct k_exec_image *image, k_size_t source, k_size_t end)
{
    K_UNUSED(image);

    if (end < source) {
        return false;
    }

    /* The image and its arguments stay below the guard page of the stack. */
    return end <= (k_size_t)PORT_EXEC_GUARD_OFFSET;
}

/* Give `len` bytes of the window a frame of the arena, page by page. */
static bool space_map(k_u64_t space, k_size_t offset, k_size_t len)
{
    k_size_t at;

    for (at = 0; at < len; at += PAGE_SIZE) {
        k_u64_t frame = port_x86_64_frame_alloc();

        if (frame == 0) {
            kerror("x86_64: no physical page for the program");
            return false;
        }

        if (port_x86_64_paging_map_user(space, PORT_EXEC_BASE + offset + at, frame, PAGE_SIZE) !=
            0) {
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
     * present PTE is the answer, and it covers copy-on-write pages too.
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
     * Both vectors live below the guard page of the stack; the one that ends
     * last decides. A zero vector occupies nothing and passes.
     */
    if (arg_end > (k_size_t)PORT_EXEC_GUARD_OFFSET || env_end > (k_size_t)PORT_EXEC_GUARD_OFFSET) {
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
     */
    if (!space_map(space, 0, align_up(end, PAGE_SIZE))) {
        goto fail;
    }

    if (user_argc > 0 || image->envp != NULL) {
        k_size_t vectors_off = (user_argc > 0) ? arg_off : env_off;
        k_size_t vectors_end = (env_end > arg_end) ? env_end : arg_end;
        k_size_t first = align_down(vectors_off, PAGE_SIZE);
        k_size_t last = align_up(vectors_end, PAGE_SIZE);

        if (!space_map(space, first, last - first)) {
            goto fail;
        }
    }

    if (!space_map(space, PORT_EXEC_STACK_OFFSET, PORT_EXEC_STACK_SIZE)) {
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

    program = task_create(PORT_EXEC_TASK_NAME, port_exec_program_task, start, TASK_PRIO_DEFAULT);
    if (program == NULL) {
        k_port_free(start);
        goto fail;
    }

    /*
     * The caller of execve waits for the program the way a parent waits for
     * a child: the program is a task of the scheduler, not a call that stays
     * on this stack. A program that returns a status goes through exit, and
     * a program that faults is taken down with 255 by the fault handler.
     */
    rc = task_wait(program->pid, &status, PORT_EXEC_WAIT_MS);
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

void port_x86_64_program_fault(k_u64_t vector, k_u64_t address)
{
    klog(K_LOG_LEVEL_ERROR, "the program faulted: vector %u at 0x%08x%08x", (unsigned)vector,
         (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu));

    /* A fault of a program is the end of its task, not of the machine. */
    task_exit(255);

    /* Not reached: the exit of a task does not come back. */
    for (;;) {
    }
}
