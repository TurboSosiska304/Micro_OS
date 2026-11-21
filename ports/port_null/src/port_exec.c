/*
 * Programs on the port that is not a machine: there is no memory to place one
 * in and no processor to jump to it. The stubs exist so that the kernel and
 * the portable components link without taking a symbol from a real machine.
 */
#include "kernel/port/exec.h"

bool k_port_exec_supported(void)
{
    return false;
}

void *k_port_exec_base(void)
{
    return NULL;
}

int k_port_exec_enter(const struct k_exec_image *image, char *const argv[])
{
    K_UNUSED(image);
    K_UNUSED(argv);
    return -1;
}

bool k_port_exec_active(void)
{
    return false;
}

void k_port_exec_exit(int code)
{
    K_UNUSED(code);
}

/* No address space per task, so no pointer of a caller to check either. */
bool k_port_exec_contains(const void *ptr, k_size_t size)
{
    K_UNUSED(ptr);
    K_UNUSED(size);
    return true;
}

/* No address space per task on the port that is not a machine. */
int k_port_fork_copy(struct task *child)
{
    K_UNUSED(child);
    return -1;
}
