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

/*
 * The simulator does not enter programs, but the machine under it has a name:
 * on x86-64 it is the machine of a program of the port of QEMU, so the kernel
 * answers ENOEXEC for an image of the board instead of ENOSYS for every file.
 * A host of another architecture has no machine of the format to name, and -1
 * leaves the answer to k_port_exec_supported() (ENOSYS).
 */
int k_port_exec_arch(void)
{
#if defined(__x86_64__)
    return (int)K_EXEC_ARCH_X86_64;
#else
    return -1;
#endif
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
 * The simulator has no address space per program: a program is a task of the
 * kernel and every pointer of a call is memory the kernel already owns. The
 * range check has nothing to check here, and answering "yes" keeps the host
 * build the reference for the calls that are not about translation.
 */
bool k_port_exec_contains(const void *ptr, k_size_t size)
{
    K_UNUSED(ptr);
    K_UNUSED(size);
    return true;
}

/*
 * A copy into the memory of a program is a copy into the memory of the
 * kernel here (no SMAP on this machine), so the window is empty.
 */
void k_port_exec_user_access_begin(void) {}

void k_port_exec_user_access_end(void) {}

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
