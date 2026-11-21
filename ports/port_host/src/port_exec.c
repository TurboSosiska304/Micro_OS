/*
 * Programs on the host: none.
 *
 * The simulator is a process of the development machine, not a machine of its
 * own. Jumping into the bytes of a program would run arbitrary code inside the
 * address space of the tests with the libc of the host under it, so -1 is the
 * only honest answer even though the process could technically do it. The
 * format and the loader are exercised here as data; the jump belongs to the
 * x86-64 port, where the kernel owns the machine.
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
 * A fork of a program: there is no address space per task here, so the child
 * is the placeholder of the kernel - a task that re-enters the program of its
 * parent over the same memory.
 */
int k_port_fork_copy(struct task *child)
{
    K_UNUSED(child);
    return -1;
}
