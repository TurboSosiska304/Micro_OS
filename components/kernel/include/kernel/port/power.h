/*
 * Micro OS — platform port: power.
 *
 * The two ends of the board: it goes down and it comes back up. What
 * "down" means for the port is its own business (the idle loop of the
 * chip, the exit of the host process), as long as it does not return.
 */
#ifndef MICRO_OS_KERNEL_PORT_POWER_H
#define MICRO_OS_KERNEL_PORT_POWER_H

/* Stop the CPU forever (used by panic and by idle). */
void k_port_halt(void);

/* Reset the board. Does not return. */
void k_port_reset(void);

#endif /* MICRO_OS_KERNEL_PORT_POWER_H */
