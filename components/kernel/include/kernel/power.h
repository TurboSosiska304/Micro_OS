/*
 * Micro OS — the two ends of the board, as the kernel sees them.
 *
 * The shell says "reboot", the panic handler stops the machine: both go
 * through here. The upper layers do not reach for the port, and there is
 * one place where the promise "this does not return" is written down.
 */
#ifndef MICRO_OS_KERNEL_POWER_H
#define MICRO_OS_KERNEL_POWER_H

/* Restart the board. Does not return. */
_Noreturn void k_reboot(void);

/* Stop the CPU forever. Does not return. */
_Noreturn void k_halt(void);

#endif /* MICRO_OS_KERNEL_POWER_H */
