/*
 * Micro OS — platform port: the programs.
 *
 * Without an MMU an image cannot be relocated, so the port names one window
 * of memory (k_port_exec_base) where a program may be placed and copies the
 * image there. The address is a property of the machine and stays fixed for
 * the life of the port; a port with no such window answers "no" and execve
 * becomes ENOSYS, which the shell can explain in plain words.
 *
 * This is the second duty of the port that is not a device (the first is the
 * task substrate): what "run this code" means is different enough between a
 * board whose flash is not executable and a machine with RAM and an MMU that
 * it cannot be written once for all of them.
 */
#ifndef MICRO_OS_KERNEL_PORT_EXEC_H
#define MICRO_OS_KERNEL_PORT_EXEC_H

#include "kernel/exec.h"

/* Can this machine run a program loaded from a file at all? */
bool k_port_exec_supported(void);

/*
 * The first address of the window of the programs, or NULL when the machine
 * has none. The kernel checks the image against this address before it asks
 * the port to enter it.
 */
void *k_port_exec_base(void);

/*
 * Copy the image to the window, clear the bss, make `argv` (NULL for no
 * arguments) the vector of the program and transfer control to `entry`.
 * Does not return when the program is entered; returns -1 when the image
 * cannot be placed or the machine cannot jump.
 */
int k_port_exec_enter(const struct k_exec_image *image, char *const argv[]);

#endif /* MICRO_OS_KERNEL_PORT_EXEC_H */
