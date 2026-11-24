/*
 * Micro OS — platform port: power.
 *
 * The ends of the board: it stops, it goes off and it comes back up. What
 * each of the three means for a port is its own business (the idle loop of
 * the chip, the S5 of ACPI, the exit of the host process), as long as the
 * call does not return. The clean end - the tasks, the volumes and the
 * flush of the disks - is the kernel's (kernel/power.h), and the port is
 * asked only for the last step.
 */
#ifndef MICRO_OS_KERNEL_PORT_POWER_H
#define MICRO_OS_KERNEL_PORT_POWER_H

/*
 * What the machine is asked to become. The three are not one call with a
 * flag, because a port may have one of them and not another: a board with
 * a power latch goes off, a machine of QEMU is ended by its monitor, and a
 * process of Linux is an exit code.
 */
enum k_power_action {
    /* Stop the processor and stay powered: the halt of panic and of idle. */
    K_POWER_HALT = 0,
    /* Remove the power (S5, the latch of the board, the exit of a process). */
    K_POWER_OFF = 1,
    /* Start again from the reset vector. */
    K_POWER_REBOOT = 2,
};

/* Stop the CPU forever (used by panic and by idle). */
void k_port_halt(void);

/* Reset the board. Does not return. */
void k_port_reset(void);

/*
 * The last step of the clean end (kernel/power.h): do what the action
 * names. Does not return on success - a port that comes back from it is a
 * broken port and the kernel stops with a panic. The platform says in its
 * log which mechanism it used and why: ACPI S5, the power latch of the
 * board, the exit code of the process.
 */
void k_port_power(enum k_power_action action);

#endif /* MICRO_OS_KERNEL_PORT_POWER_H */
