/*
 * Programs on the ESP32: there is no way to run one from a file.
 *
 * The flash is not executable for code the kernel did not link into the
 * image, and a copy in internal SRAM would have to live in the ~100 KB that
 * the kernel, the drivers and the stacks already share. The port does not
 * pretend: execve returns ENOSYS on this machine, and the shell says why.
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

/*
 * The board has no address space per task: a program would be a task of the
 * kernel and its pointers would be the kernel's own. Every range is accepted,
 * exactly as on a machine where the pointer check has nothing to check.
 */
bool k_port_exec_contains(const void *ptr, k_size_t size)
{
    K_UNUSED(ptr);
    K_UNUSED(size);
    return true;
}

/* Without an MMU the child shares the memory: the kernel's placeholder. */
int k_port_fork_copy(struct task *child)
{
    K_UNUSED(child);
    return -1;
}
