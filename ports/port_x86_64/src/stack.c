/*
 * Micro OS — the x86-64 port: the guard page under the stack of a task.
 *
 * The stack of a kernel task has a page under it that is not present (ADR
 * 0048). The recursion below puts the stack pointer past that page, the
 * processor faults in ring 0, and the handler of the fault recognizes the
 * address as the guard of the task that runs and ends that task with status
 * 255 instead of stopping the machine. The command is the proof a person can
 * type: the task dies, the shell that waited for it answers the next line.
 *
 * The file is a part of the port and not of the shell: the shell registers
 * the command (kshell_register) and knows nothing else about it, which is the
 * same shape as `smp` of src/smp.c and the commands of the network.
 */
#include "port_x86_64.h"

#include "kernel/log.h"
#include "kernel/task.h"
#include "kshell/kshell.h"

/*
 * The bytes a frame of the recursion keeps alive. The array is volatile so
 * that the compiler cannot throw it away or turn the call into a loop: a
 * function whose only work is a call to itself is a jump at -O2, and a jump
 * down the stack never moves the stack pointer. The write after the call is
 * the second half of the same guard.
 */
#define STACK_FRAME_BYTES 512u
#define STACK_MAX_DEPTH 100000u

static volatile k_u32_t g_stack_sink;

static __attribute__((noinline)) void stack_overrun(k_u32_t depth)
{
    volatile k_u8_t pad[STACK_FRAME_BYTES];

    pad[0] = (k_u8_t)(depth & 0xFFu);
    pad[STACK_FRAME_BYTES - 1u] = (k_u8_t)((depth >> 8) & 0xFFu);

    if (depth < STACK_MAX_DEPTH) {
        stack_overrun(depth + 1u);
    }

    /* Read after the call, so the call is not the last thing of the frame
     * and the recursion cannot become a loop. */
    g_stack_sink = (k_u32_t)g_stack_sink + pad[0];
}

static void stack_overrun_task(void *arg)
{
    K_UNUSED(arg);

    stack_overrun(0);

    /* A stack that held the whole recursion is a guard that did not fire:
     * the task ends normally and the command says so. */
    kwarn("x86_64: the stack of the test task held %u frames", (unsigned)STACK_MAX_DEPTH);
}

static int cmd_guard(int argc, char **argv)
{
    struct task *task;
    k_u32_t pid;
    int status = -1;
    int rc;

    K_UNUSED(argc);
    K_UNUSED(argv);

    task = task_create("stack-test", stack_overrun_task, NULL, TASK_PRIO_DEFAULT);
    if (task == NULL) {
        kshell_printf("guard: cannot create the task\n");
        return KSHELL_ERROR;
    }

    /*
     * The pid is read before the wait: a parent may reap a zombie at any
     * moment, and the slot - with the pid in it - is zeroed by the reaping.
     */
    pid = task->pid;
    rc = task_wait(pid, &status, 10000);

    if (rc == 0 && status == 255) {
        kshell_printf("guard: the task ran into the guard page and died (status 255); the "
                      "machine is alive\n");
        return KSHELL_OK;
    }

    kshell_printf("guard: the task did not end in the guard page (rc %d, status %d)\n", rc, status);

    return KSHELL_ERROR;
}

int port_x86_64_stack_register(void)
{
    return kshell_register("guard", "prove a stack overflow kills the task, not the machine",
                           cmd_guard);
}
