/*
 * Micro OS — the factory reset of the board.
 *
 * A factory reset is what a person does before handing the board over or
 * after breaking the configuration past the point of a fix: the volume of
 * the flash goes away and so does the store that keeps /etc and the saved
 * network. The firmware itself and the bootloader are not touched — they
 * live in partitions of their own — so the board comes back as a board that
 * has never been used, with the same firmware.
 *
 * The command refuses without -y: there is no undo, and a shell in the
 * hand of somebody who did not read the line is not a confirmation.
 */
#ifndef MICRO_OS_FACTORY_H
#define MICRO_OS_FACTORY_H

/*
 * Erase the volume and the store and restart the board. Does not return on
 * success; a negative answer is the step that failed and the board stays up
 * with the message of the command on the console.
 */
int factory_reset(void);

/* Register the `factory` command of the shell. */
int factory_register(void);

#endif /* MICRO_OS_FACTORY_H */
