/*
 * Micro OS — the shell over the air.
 *
 * A telnet server for the local network: the same shell the UART and
 * the display show, on port 23, behind the accounts of /etc. A client is
 * asked for a name and a password (three attempts, `/etc/passwd` and
 * `/etc/shadow`), the session runs as that user, and `logout` ends it; the
 * password is read from the socket and never repeated (ADR 0033, the second
 * half of the accounts). The command `telnet off` closes the door.
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
