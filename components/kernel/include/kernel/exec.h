/*
 * Micro OS — the format of a program.
 *
 * A program is a flat image linked for the address the port names
 * (k_port_exec_base): without an MMU there is nothing to relocate, so the
 * image is copied there as it is. The format is ours and not ELF because the
 * loader of Linux is a chapter of its own, and the smallest machine of the
 * project has about 100 KB of SRAM for everything, drivers included.
 *
 * The header is little endian and every field is a k_u32_t:
 *
 *    0  magic      the bytes 'M', 'O', 'X', '1'
 *    4  version    1
 *    8  flags      0, a flag this kernel does not know is a refusal
 *   12  entry      offset of the first instruction from the start of the image
 *   16  text_size  bytes of code
 *   20  data_size  bytes of initialised data
 *   24  bss_size   bytes of zeroed data, cleared by the port before the entry
 *   28  reserved   0
 *   32  ...        the code, then the data
 *
 * The sizes do not count the header, and they are multiples of four. An image
 * may carry padding after the data; the parse ignores it.
 */
#ifndef MICRO_OS_KERNEL_EXEC_H
#define MICRO_OS_KERNEL_EXEC_H

#include "kernel/types.h"

/* The magic as the four bytes of the file read as a little-endian word. */
#define K_EXEC_MAGIC 0x31584F4Du
#define K_EXEC_VERSION 1u
#define K_EXEC_HEADER_SIZE 32u

/*
 * The zeroed part is not in the file, so nothing limits it but the memory of
 * the machine; a megabyte is far more than a program of this system needs and
 * keeps a corrupt header from asking for all of the RAM.
 */
#define K_EXEC_BSS_MAX (1024u * 1024u)

/*
 * A parsed image: the bytes stay where the caller has them (the page of the
 * file), the sizes are the ones of the header. `entry` is an offset from the
 * start of the image, header included.
 *
 * `envp` is not in the file and not of the parser: it is the environment the
 * caller of the run hands the program, a vector of "NAME=VALUE" strings
 * terminated by a NULL. The parser sets it to NULL, which means "the
 * environment of the system" (k_exec_environ); a caller that copies one from
 * its own memory - execve does - sets it before k_exec_run. The strings have
 * to stay alive for as long as the program runs, exactly as the strings of
 * argv do.
 */
struct k_exec_image {
    const void *data;
    k_size_t size;
    k_size_t entry;
    k_size_t text_size;
    k_size_t data_size;
    k_size_t bss_size;
    const char *const *envp;
};

/*
 * Check the header of a file and fill `out`. Returns 0 when the image is
 * consistent, -1 when it is not: a wrong magic or version, sizes that do not
 * fit the file, an entry outside the text, an unaligned size, an unknown flag
 * or a bss that is too large. `out` is written only on success; the bytes of
 * the image are never modified.
 *
 * This call checks the image against itself, not against the machine: whether
 * the image fits where the port puts a program is the business of k_exec_run.
 */
int k_exec_parse(const void *data, k_size_t size, struct k_exec_image *out);

/*
 * Hand a parsed image to the port that runs it. A port that enters a program
 * that never comes back does not return at all, so there are two failures and
 * one success: -1 when the machine refused the image (it does not fit the
 * window of the port, or the port could not copy it), -2 when the machine
 * cannot run a program at all - the kernel turns that into ENOSYS - and a
 * number from 0 to 255 when the program returned. That number is the status
 * the program ended with, the low byte of what it left in the register of the
 * result, and it is not an error: a program that finishes has to be noticed
 * on a machine whose execve has no process to exit into.
 *
 * `argv` is the vector the program starts with, its pointers into memory the
 * caller keeps for as long as the program runs; NULL is a program with no
 * arguments. The environment comes from `image->envp` and a NULL there is the
 * environment of the system (k_exec_environ), so a program always starts with
 * a name it can look for a value in. The kernel does not walk either vector -
 * without an MMU there is nothing to check an array of pointers against, and a
 * program that never returns keeps the caller's frame alive anyway. A call
 * that takes a vector from the memory of a program (execve) copies it
 * somewhere that outlives the call first: kernel/ksys.c uses the frame of the
 * call for that.
 */
int k_exec_run(const struct k_exec_image *image, char *const argv[]);

/*
 * The environment of the system: what a program started without one sees. It
 * is a vector of "NAME=VALUE" words terminated by NULL, and it is static
 * memory of the kernel, so a caller keeps it only for as long as it runs
 * something that reads it.
 *
 * The shell has no `export` yet, so this is the environment of every program
 * of the machine: the tree of the programs and the home of the one user of
 * the system. A program that was given one of its own sees that one instead;
 * the port copies whichever vector the call carries into the memory of the
 * program.
 */
const char *const *k_exec_environ(void);

/*
 * The status a program ended with, reported by the port when the program
 * returns and before k_port_exec_enter() returns 0. The port owns the moment
 * (it is the one that receives control back), the kernel owns the number
 * (it is the one that hands it to the caller of execve), and this call is
 * the seam between the two. Outside of a run it is a no-op that no caller
 * should need; a program that is entered and never returns never calls it.
 */
void k_exec_exit(int code);

#endif /* MICRO_OS_KERNEL_EXEC_H */
