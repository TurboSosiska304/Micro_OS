/*
 * Micro OS — the task substrate of the x86-64 port, first milestone.
 *
 * The kernel schedules tasks on top of the substrate the port provides.
 * That substrate is a context switch: the callee-saved registers and the
 * stack pointer of a task, and the timer interrupt that decides whose turn
 * is next. It is the next milestone of the port (ADR 0002).
 *
 * Until it exists the port says "there is no scheduler here" instead of
 * pretending, and the kernel reports it through the usual path.
 */
#include "kernel/port_task.h"

#include "kernel/types.h"

k_size_t k_port_task_stack_size(void)
{
    return 16 * 1024;
}

int k_port_task_create(struct task *t, k_port_task_entry_fn entry, void *arg)
{
    K_UNUSED(t);
    K_UNUSED(entry);
    K_UNUSED(arg);
    return -1;
}

void k_port_task_start(struct task *t)
{
    K_UNUSED(t);
}

void k_port_task_yield(void) {}

void k_port_task_sleep_ms(k_u32_t ms)
{
    K_UNUSED(ms);
}

void k_port_task_exit(int code)
{
    K_UNUSED(code);
}

struct task *k_port_task_current(void)
{
    return NULL;
}
