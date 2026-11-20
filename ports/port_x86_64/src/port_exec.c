/*
 * Micro OS — the x86-64 port: running a program.
 *
 * A program is a user of the machine now (ADR 0007). It has page tables of
 * its own, its own stack in the window of the programs, and the processor
 * drops to ring 3 before its first instruction: the kernel is above it and
 * out of reach, and the only way back in is `int $0x80` through a gate that
 * opens for ring 3.
 *
 * The window starts at 32 MiB and every page of it that the program may use
 * is backed by the physical arena at 64 MiB. The kernel reaches the bytes
 * through the identity map, the program through its page tables, and an
 * address the program computes is not an address of the kernel.
 *
 * Two duties meet here: placing the image (the pages, the copy of text and
 * data, the zeroed bss, the arguments) and the crossing itself. The crossing
 * is the interesting half. Before it the caller is ordinary kernel code on a
 * kernel stack; after it the processor runs at ring 3. The return is not a
 * `ret`: a program that calls `exit` (or faults) unwinds the call of
 * port_exec_jump_user() from wherever it stands and lands in
 * k_port_exec_enter() with the status, because a program on this port is not
 * a task of the scheduler and has no context to switch to.
 */
#include <string.h>

#include "kernel/log.h"
#include "kernel/port.h"
#include "kernel/port/exec.h"
#include "port_x86_64.h"

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
 * The stack the processor takes when the program calls the kernel or is
 * interrupted: RSP0 of the TSS points at its top. It is separate from the
 * kernel stack of the caller because the call of k_port_exec_enter() stays
 * alive below it for as long as the program runs.
 */
#define PORT_EXEC_KSTACK_SIZE (32u * 1024u)

/*
 * The state of the kernel while the program runs. It lives in variables of
 * the file and not in locals because the assembly that enters and leaves the
 * program finds them by symbol; the stack of the kernel and its callee-saved
 * registers are saved in them.
 */
k_u64_t g_port_exec_rsp;
k_u64_t g_port_exec_kstack_top;
k_u64_t g_port_exec_kernel_cr3;
k_u64_t g_port_exec_program_cr3;
k_u64_t g_port_exec_exit_code;

static k_u8_t g_program_kstack[PORT_EXEC_KSTACK_SIZE] __attribute__((aligned(16)));
static volatile bool g_port_exec_active;
static bool g_port_exec_faulted;

/*
 * void port_exec_jump_user(void *entry, k_u64_t argc, char *const *argv,
 *                          k_u64_t user_rsp)
 *
 * Save the kernel context, put the processor on the kernel stack of the
 * program and return to ring 3 through an iretq frame: SS, RSP, RFLAGS, CS
 * and RIP, in the order the instruction pops them. The user selectors are
 * 0x23 (data, DPL 3) and 0x1b (code, DPL 3) of the GDT of gdt.c.
 *
 * The program's first instruction sees the arguments of a C function - the
 * count in RDI, the vector in RSI - on a stack whose top word is zero, so a
 * `ret` that no program of the system writes lands on address zero and
 * faults instead of running through memory.
 *
 * port_exec_return() is the other half. It is called from the syscall or the
 * fault handler, restores the kernel context saved above and returns from
 * the call of port_exec_jump_user() into k_port_exec_enter(). It never
 * returns to its own caller, which is a frame of the kernel stack that the
 * program's exit makes irrelevant.
 */
void port_exec_jump_user(void *entry, k_u64_t argc, char *const *argv, k_u64_t user_rsp);
void port_exec_return(void);

__asm__(".section .text, \"ax\", @progbits\n"
        ".globl port_exec_jump_user\n"
        ".type port_exec_jump_user, @function\n"
        "port_exec_jump_user:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, g_port_exec_rsp(%rip)\n"
        "    movq %rdi, %r12\n" /* the entry of the program */
        "    movq %rsi, %r13\n" /* argc */
        "    movq %rdx, %r14\n" /* argv */
        "    movq %rcx, %r15\n" /* the stack of the program */
        "    andq $-16, %rsp\n"
        "    call port_x86_64_tls_user\n"
        "    movq g_port_exec_kstack_top(%rip), %rsp\n"
        "    pushq $0x23\n"  /* SS, ring 3 data */
        "    pushq %r15\n"   /* RSP of the program */
        "    pushq $0x202\n" /* RFLAGS: interrupts on, the required bit 1 */
        "    pushq $0x1b\n"  /* CS, ring 3 code */
        "    pushq %r12\n"   /* RIP */
        "    movq %r13, %rdi\n"
        "    movq %r14, %rsi\n"
        "    xorl %eax, %eax\n"
        "    xorl %ebx, %ebx\n"
        "    xorl %ecx, %ecx\n"
        "    xorl %edx, %edx\n"
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
        ".size port_exec_jump_user, . - port_exec_jump_user\n"
        "\n"
        ".globl port_exec_return\n"
        ".type port_exec_return, @function\n"
        "port_exec_return:\n"
        "    andq $-16, %rsp\n"
        "    call port_x86_64_tls_kernel\n" /* the kernel's errno is back */
        "    movq g_port_exec_kernel_cr3(%rip), %rax\n"
        "    movq %rax, %cr3\n"
        "    movq g_port_exec_rsp(%rip), %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    cld\n"
        "    ret\n"
        ".size port_exec_return, . - port_exec_return\n");

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

bool k_port_exec_supported(void)
{
    return true;
}

void *k_port_exec_base(void)
{
    return (void *)(uintptr_t)PORT_EXEC_BASE;
}

bool k_port_exec_active(void)
{
    return g_port_exec_active;
}

void k_port_exec_exit(int code)
{
    if (!g_port_exec_active) {
        return;
    }

    g_port_exec_exit_code = (k_u64_t)(code & 0xFF);
    g_port_exec_active = false;
    port_exec_return();
}

int k_port_exec_enter(const struct k_exec_image *image, char *const argv[])
{
    k_u64_t space;
    k_u64_t user_rsp;
    k_u64_t user_argc = 0;
    k_u64_t user_argv = 0;
    k_size_t source;
    k_size_t end;
    k_size_t arg_off = 0;
    k_size_t arg_end = 0;
    k_size_t strings = 0;
    k_size_t array_off = 0;
    k_size_t index;
    k_u8_t *physical;
    int rc;

    if (g_port_exec_active) {
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

    if (user_argc > 0) {
        arg_off = align_up(end, 16u);
        array_off = align_up(arg_off + strings, 8u);
        arg_end = array_off + (k_size_t)(user_argc + 1u) * sizeof(k_u64_t);

        if (arg_end > (k_size_t)PORT_EXEC_GUARD_OFFSET) {
            kerror("x86_64: the arguments do not fit below the stack of the program");
            return -1;
        }
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
    rc = port_x86_64_paging_map_user(PORT_EXEC_BASE, PORT_EXEC_PHYS, align_up(end, PAGE_SIZE));
    if (rc != 0) {
        goto fail;
    }

    if (user_argc > 0) {
        k_size_t first = align_down(arg_off, PAGE_SIZE);
        k_size_t last = align_up(arg_end, PAGE_SIZE);

        rc = port_x86_64_paging_map_user(PORT_EXEC_BASE + first, PORT_EXEC_PHYS + first,
                                         last - first);
        if (rc != 0) {
            goto fail;
        }
    }

    rc = port_x86_64_paging_map_user(PORT_EXEC_BASE + PORT_EXEC_STACK_OFFSET,
                                     PORT_EXEC_PHYS + PORT_EXEC_STACK_OFFSET, PORT_EXEC_STACK_SIZE);
    if (rc != 0) {
        goto fail;
    }

    /*
     * The bytes live in the physical arena; the program sees them through its
     * page tables at the base of the window. The header is copied with the
     * segments because the entry of the image is an offset from its start.
     */
    physical = (k_u8_t *)port_x86_64_paging_phys(PORT_EXEC_BASE);
    if (physical == NULL) {
        goto fail;
    }

    memcpy(physical, image->data, source);

    if (image->bss_size != 0) {
        memset(physical + source, 0, image->bss_size);
    }

    if (user_argc > 0) {
        k_u8_t *strings_at = physical + arg_off;
        k_u64_t *array = (k_u64_t *)(physical + array_off);
        k_size_t cursor = arg_off;

        for (index = 0; index < (k_size_t)user_argc; index++) {
            k_size_t length = (k_size_t)strlen(argv[index]) + 1u;

            memcpy(strings_at, argv[index], length);
            array[index] = PORT_EXEC_BASE + (k_u64_t)cursor;
            strings_at += length;
            cursor += length;
        }

        array[user_argc] = 0;
        user_argv = PORT_EXEC_BASE + (k_u64_t)array_off;
    }

    /*
     * The stack of the program starts as zeros: the arena is the same memory
     * for the next program, and what the last one left on its stack is not
     * the business of this one. The top word is the zero a `ret` would pop.
     */
    memset(port_x86_64_paging_phys(PORT_EXEC_BASE + PORT_EXEC_STACK_OFFSET), 0,
           PORT_EXEC_STACK_SIZE);
    user_rsp = PORT_EXEC_BASE + PORT_EXEC_STACK_OFFSET + PORT_EXEC_STACK_SIZE - sizeof(k_u64_t);
    *(k_u64_t *)port_x86_64_paging_phys(user_rsp) = 0;

    /*
     * The processor takes RSP0 on the first call of the program and on the
     * first interrupt of it. The stack belongs to this program and is empty
     * at the top: the kernel frames of k_port_exec_enter() stay on the stack
     * of the caller, below the point where the program can reach them.
     */
    g_port_exec_kstack_top =
        (k_u64_t)(uintptr_t)(g_program_kstack + PORT_EXEC_KSTACK_SIZE) & ~(k_u64_t)15u;
    port_x86_64_tss_set_rsp0(g_port_exec_kstack_top);

    g_port_exec_kernel_cr3 = port_x86_64_kernel_cr3();
    g_port_exec_program_cr3 = space;
    g_port_exec_exit_code = 0;
    g_port_exec_faulted = false;
    g_port_exec_active = true;

    port_x86_64_paging_switch(space);
    port_exec_jump_user((void *)(uintptr_t)(PORT_EXEC_BASE + image->entry), user_argc,
                        (char *const *)(uintptr_t)user_argv, user_rsp);

    /*
     * Reached only because the program ended - by `exit`, by a fault, or by a
     * `ret` that landed on address zero. The return stub has put the kernel's
     * CR3 and stack back; the program's tables and stack are dead now.
     */
    g_port_exec_active = false;
    port_x86_64_paging_space_destroy(space);

    if (g_port_exec_faulted) {
        /* The fault line has already been printed by the handler. */
        k_exec_exit(255);
        return 0;
    }

    k_exec_exit((int)(g_port_exec_exit_code & 0xFFu));
    kinfo("x86-64: the program exited with %d", (int)(g_port_exec_exit_code & 0xFFu));

    return 0;

fail:
    port_x86_64_paging_space_destroy(space);
    return -1;
}

void port_x86_64_program_fault(k_u64_t vector, k_u64_t address)
{
    if (!g_port_exec_active) {
        /* A fault that is not a program's: the caller of this function does
         * not exist to return to, and stopping is the honest answer. */
        k_port_halt();
    }

    klog(K_LOG_LEVEL_ERROR, "the program faulted: vector %u at 0x%08x%08x", (unsigned)vector,
         (unsigned)(address >> 32), (unsigned)(address & 0xFFFFFFFFu));

    g_port_exec_faulted = true;
    g_port_exec_exit_code = 255;
    g_port_exec_active = false;
    port_exec_return();
}
