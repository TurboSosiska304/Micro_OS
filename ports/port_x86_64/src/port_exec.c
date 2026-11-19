/*
 * Programs on the x86-64 port: this is the machine that can run one.
 *
 * The window starts at 32 MiB. The kernel is linked at 16 MiB and the heap of
 * the port is cut below the window (see heap_init in port_x86_64.c), and the
 * identity map of the loader covers the first gigabyte, so this address means
 * the same thing to an image and to the processor.
 */
#include "kernel/port/exec.h"

#define PORT_EXEC_BASE 0x02000000u

bool k_port_exec_supported(void)
{
    return true;
}

void *k_port_exec_base(void)
{
    return (void *)(uintptr_t)PORT_EXEC_BASE;
}

int k_port_exec_enter(const struct k_exec_image *image, char *const argv[])
{
    K_UNUSED(image);
    K_UNUSED(argv);

    /* the jump lands in the next milestone */
    return -1;
}
