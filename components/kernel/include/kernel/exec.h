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
 *    4  version    2
 *    8  flags      0, a flag this kernel does not know is a refusal
 *                  (K_EXEC_FLAG_WX names the one flag the format knows and
 *                  still refuses: a page both writable and executable)
 *   12  entry      offset of the first instruction from the start of the image
 *   16  text_size  bytes of code
 *   20  data_size  bytes of initialised data
 *   24  bss_size   bytes of zeroed data, cleared by the port before the entry
 *   28  arch       the machine the bytes are code for: 0 x86-64, 1 Xtensa
 *   32  ...        the code, then the data
 *
 * The sizes do not count the header, and they are multiples of four. An image
 * may carry padding after the data; the parse ignores it.
 *
 * Version 2 is version 1 with the machine in the last field, which version 1
 * kept reserved and zero. The bytes of one machine are not the bytes of
 * another - an x86-64 image is not Xtensa code - so the machine travels in
 * the file and the kernel refuses a program of the other one before a byte of
 * it runs (ADR 0018). The header of version 1 is not read: a kernel that
 * guessed which machine an old image was for would guess wrong, so the
 * parser asks for the version it knows and says which one that is.
 */
#ifndef MICRO_OS_KERNEL_EXEC_H
#define MICRO_OS_KERNEL_EXEC_H

#include "kernel/types.h"

/* The magic as the four bytes of the file read as a little-endian word. */
#define K_EXEC_MAGIC 0x31584F4Du
#define K_EXEC_VERSION 2u
#define K_EXEC_HEADER_SIZE 32u

/*
 * The only flag the format has ever named: an image that asks for one page
 * both writable and executable. No port of this system builds such a page -
 * the text is read-only and executable, the data writable and NX (ADR 0007,
 * ADR 0048) - so the parser refuses the request with a line of its own
 * instead of the generic "unknown flags". The bit was unknown before this
 * decision, which means an image that set it was refused then too: naming it
 * costs no compatibility.
 */
#define K_EXEC_FLAG_WX 0x1u
#define K_EXEC_FLAGS_KNOWN K_EXEC_FLAG_WX

/*
 * The machine of an image, the number of the field `arch`. The list is the
 * list of the ports that run programs: x86-64 (the port of QEMU) and Xtensa
 * (the board). A number outside the two is not a machine this format knows,
 * and the parser refuses it instead of carrying it to a port that would have
 * to guess.
 */
#define K_EXEC_ARCH_X86_64 0u
#define K_EXEC_ARCH_XTENSA 1u

/*
 * The zeroed part is not in the file, so nothing limits it but the memory of
 * the machine; a megabyte is far more than a program of this system needs and
 * keeps a corrupt header from asking for all of the RAM.
 */
#define K_EXEC_BSS_MAX (1024u * 1024u)

/*
 * The largest image a call of the system reads and hands to a port. It is a
 * cap of the memory of the machine, not of the format: the image is read in
 * pieces of KSYS_MAX_IO (64 KiB) into one block of the allocator, and the
 * block comes from the PSRAM on the board and from the heap of the port on
 * x86-64. Eight megabytes hold the interpreter of Lua (164 KiB), the library
 * of SQLite and the page of a program of the future; a file past it is
 * refused before a byte is read, with E2BIG.
 *
 * The window of a port is the other limit: an image that fits here but not
 * the window of the machine is refused by the port with ENOMEM. The two
 * numbers are independent and neither is the transfer edge of one call.
 */
#define K_EXEC_MAX_IMAGE (8u * 1024u * 1024u)

/*
 * The size of a file against the two limits of the format and of the call:
 * 0 when a call may read it, KERR_INVAL when it cannot hold the header, and
 * KERR_TOOBIG when it is past K_EXEC_MAX_IMAGE. The answers are the ones of
 * the system calls, so a caller returns them as they are.
 */
int k_exec_image_check(k_size_t size);

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
    /* The machine of the header: K_EXEC_ARCH_X86_64 or K_EXEC_ARCH_XTENSA. */
    k_u32_t arch;
    k_size_t entry;
    k_size_t text_size;
    k_size_t data_size;
    k_size_t bss_size;
    const char *const *envp;
    /*
     * The file the image is in, or NULL when `data` holds the bytes. A port
     * with page tables maps the file and takes the pages on the fault of the
     * program (kernel/mm.h): the image is never read into a buffer of the
     * kernel, and a program of 4 MiB costs the pages it runs and not a second
     * copy of itself. A port without one reads `data` as it always did.
     */
    const char *path;
};

/*
 * Check the header of a file and fill `out`. Returns 0 when the image is
 * consistent, -1 when it is not: a wrong magic, a version this kernel does
 * not read (anything but 2), an arch that is not a machine of the format,
 * sizes that do not fit the file, an entry outside the text, an unaligned
 * size, an unknown flag or a bss that is too large. `out` is written only on
 * success; the bytes of the image are never modified.
 *
 * This call checks the image against itself, not against the machine: whether
 * the image fits where the port puts a program is the business of k_exec_run.
 */
int k_exec_parse(const void *data, k_size_t size, struct k_exec_image *out);

/*
 * The same parse of an image whose bytes were not read into a buffer: the
 * caller holds the first K_EXEC_HEADER_SIZE bytes of the file and gives the
 * size of the whole file, which is what the segments are checked against. The
 * parser reads the header and nothing else, so the answer is the one of
 * k_exec_parse; the `data` of the result is NULL and `path` is left to the
 * caller, which is how a port with page tables maps the file instead of
 * copying it (kernel/mm.h).
 */
int k_exec_parse_header(const void *header, k_size_t size, struct k_exec_image *out);

/*
 * The answers of k_exec_run() that are not the status of a program. They are
 * numbers of the call and not of the system: kernel/ksys.c is the one place
 * that turns them into the errors a program sees (ENOEXEC, ENOSYS, ENOMEM).
 */
#define K_EXEC_RUN_REFUSED (-1)    /* the machine refused the image */
#define K_EXEC_RUN_NO_MACHINE (-2) /* the machine cannot run a program at all */
#define K_EXEC_RUN_OTHER_ARCH (-3) /* the image is code for another machine */
/*
 * The image replaced the caller: the port rewrote the frame of the system
 * call and the return of the call goes to the entry of the new program, not
 * back to the code that asked. The kernel keeps the task (pid, parent,
 * descriptors) and does not report a status, because there is nobody to
 * report it to yet: the status belongs to the parent of the program, and the
 * end of the program is the end of that same task for `waitpid`.
 */
#define K_EXEC_RUN_REPLACED (-4)

/*
 * Hand a parsed image to the port that runs it. A port that enters a program
 * that never comes back does not return at all, so there are three failures
 * and two successes: K_EXEC_RUN_REFUSED when the machine refused the image (it
 * does not fit the window of the port, or the port could not copy it),
 * K_EXEC_RUN_NO_MACHINE when the machine cannot run a program at all - the
 * kernel turns that into ENOSYS - K_EXEC_RUN_OTHER_ARCH when the image names
 * a machine this port is not - the kernel turns that into ENOEXEC, and no
 * byte of the image is touched - and a number from 0 to 255 when the program
 * returned. That number is the status the program ended with, the low byte of
 * what it left in the register of the result, and it is not an error: a
 * program that finishes has to be noticed on a machine whose execve has no
 * process to exit into. K_EXEC_RUN_REPLACED is the other success: the caller
 * was a program, and the port has already made the new image the memory and
 * the entry of that same task. The call of the system does not return to the
 * old program, so the kernel answers it with 0 and lets the frame of the
 * return do the transfer.
 *
 * The machine of the image is `image->arch`, and the machine of the port is
 * k_port_exec_arch(): a port that names itself (0 or 1) runs only the images
 * of that number, and a port with no machine to name (-1) has nothing to
 * compare and answers NO_MACHINE for every image.
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
