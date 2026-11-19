/*
 * Micro OS — the task substrate of the null port.
 *
 * There is no scheduler under the kernel here: the null port exists to
 * prove that the kernel and the portable components link against the
 * contract alone, not to run anything. The calls answer "there is no task
 * to create" and the kernel reports it instead of crashing.
 */
#include "kernel/port_task.h"

#include "kernel/types.h"

k_size_t k_port_task_stack_size(void)
{
    return 0;
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
