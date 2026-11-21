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

struct task;

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

/*
 * Is a program of this port running right now? A machine that enters a
 * program and can take control back answers true for as long as the program
 * is the caller: `exit` from a program is not the end of a kernel task, it is
 * the end of that program, and the kernel has to know which of the two it is
 * looking at. A port whose programs are tasks answers true while one runs;
 * a port without a program in flight answers false.
 */
bool k_port_exec_active(void);

/*
 * End the program that is running and return to the call of
 * k_port_exec_enter() with `code` as its status (the low byte, as the status
 * of a process). Does not return while a program is active; a port with no
 * program in flight returns without doing anything. This is the substance
 * behind KSYS_EXIT on a machine whose programs are not tasks of the kernel.
 */
void k_port_exec_exit(int code);

/*
 * Is the range `ptr`..`ptr + size` memory the kernel may touch on behalf of
 * the caller? This is the seam of `copy_from_user`: the kernel must not
 * trust a pointer a program hands it, because reading an unmapped address in
 * ring 0 stops the machine instead of failing the call.
 *
 * A port whose programs share the memory of the kernel (the board, the host,
 * the null port) answers true: every pointer it is given is already the
 * kernel's own. A port with an address space per program checks the range
 * against the window that program may use, page by page, and answers false
 * for the null page, the guard page of the stack, the rest of the window that
 * was never mapped, and a range that runs past its end. A kernel task answers
 * true even on such a port: only a program has an address space to check.
 *
 * The call does not copy and does not fault; the caller copies the bytes with
 * its own `memcpy` after the range is known to be there. `size` of zero names
 * no bytes and is always true.
 */
bool k_port_exec_contains(const void *ptr, k_size_t size);

/*
 * The substance of fork() on a machine with an MMU: the kernel has made the
 * record of the child (`struct task`, a pid, the parent), and the port gives
 * it the context of the caller - the address space, the registers and the
 * point where the call returns - with the answer zero. The caller is the
 * parent and gets the pid of the child back.
 *
 * Answers the pid of the child on success, -1 when the machine cannot copy a
 * context (a machine without an address space per task, or a caller that is
 * not a program), and -2 when the copy failed for want of memory. The kernel
 * keeps its own placeholder for the -1.
 */
int k_port_fork_copy(struct task *child);

#endif /* MICRO_OS_KERNEL_PORT_EXEC_H */
