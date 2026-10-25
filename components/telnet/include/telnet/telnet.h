/*
 * Micro OS — the shell over the air.
 *
 * A telnet server for the local network: the same shell the UART and
 * the display show, on port 23. There is no authentication — the
 * board is on the network of the person who owns it, and the command
 * `telnet off` closes the door (see the banner the client gets).
 */
#ifndef MICRO_OS_TELNET_TELNET_H
#define MICRO_OS_TELNET_TELNET_H

/*
 * Register the module and the command of the shell: the listener
 * task is started by the module. Returns 0, or -1 when the task
 * could not be created.
 */
int telnet_register(void);

#endif /* MICRO_OS_TELNET_TELNET_H */
