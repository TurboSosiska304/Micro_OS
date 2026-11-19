/*
 * Micro OS — halt and reset.
 *
 * The port promises that these calls do not return. The kernel does not
 * take that on faith: a port that comes back from a reset is a broken
 * port, and the machine must not continue with half of a board.
 */
#include "kernel/power.h"

#include "kernel/panic.h"
#include "kernel/port.h"

_Noreturn void k_reboot(void)
{
    k_port_reset();

    KERNEL_PANIC("the port returned from reset");
}

_Noreturn void k_halt(void)
{
    k_port_halt();

    KERNEL_PANIC("the port returned from halt");
}
