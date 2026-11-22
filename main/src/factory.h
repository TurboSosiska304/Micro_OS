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
 * "A board that has never been used" still has the files of /etc of the
 * model, and the boot writes them whenever one is missing: a reset, a
 * formatted volume and a first boot all end in the same tree. The alarm of
 * the clock is not a file and is not erased: it lives in the BM8563, and it
 * is the wake source of a board that is off, so a reset keeps it.
 *
 * The command refuses without -y: there is no undo, and a shell in the
 * hand of somebody who did not read the line is not a confirmation.
 */
#ifndef MICRO_OS_FACTORY_H
#define MICRO_OS_FACTORY_H

/*
 * Write the files of /etc that the model has and the volume does not: the
 * setting of the board. A file that is there is left alone, so the boot can
 * call this on every volume without a word. Returns the number of the files
 * written, or a negative error when the directory cannot be made or read.
 */
int factory_defaults(void);

/*
 * Erase the volume and the store, write the defaults back and restart the
 * board. Does not return on success; a negative answer is the step that
 * failed and the board stays up with the message of the command on the
 * console.
 */
int factory_reset(void);

/* Register the `factory` command of the shell. */
int factory_register(void);

#endif /* MICRO_OS_FACTORY_H */
