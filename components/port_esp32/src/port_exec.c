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

/* Without an MMU the child shares the memory: the kernel's placeholder. */
int k_port_fork_copy(struct task *child)
{
    K_UNUSED(child);
    return -1;
}
