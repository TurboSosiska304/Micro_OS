/*
 * Micro OS — platform port: the memory a program can map.
 *
 * A machine without an MMU has no page to give a program: the board and the
 * simulator share the memory of the kernel and answer "no" here, and `mmap`
 * becomes ENOSYS for them, which is the same answer they give to `execve`.
 * A machine with page tables names a window of addresses that belong to a
 * program and hands out the pages of it on demand: the kernel asks for the
 * page behind an address when the program faults on it, fills it from the
 * file or with zeros, and the port is the only code that knows where the
 * frame is.
 *
 * The address space is the handle of the calls, not the task: a program that
 * is being placed by `execve` has no task yet, and the space of the caller is
 * the space the kernel serves a fault of. On the x86-64 port the handle is
 * the value of CR3; a port with no page tables has no handle at all and
 * answers zero.
 */
#ifndef MICRO_OS_KERNEL_PORT_MM_H
#define MICRO_OS_KERNEL_PORT_MM_H

#include "kernel/types.h"

/* The protection of a page, as the kernel names it (kernel/mm.h has the same
 * three bits under the names of the ABI). */
#define K_PORT_MM_READ 0x01u
#define K_PORT_MM_WRITE 0x02u
#define K_PORT_MM_EXEC 0x04u

/* Can this machine give a program a page of its own? */
bool k_port_mm_supported(void);

/* The window of the addresses a program may map: the first one and the first
 * one past the end. Zero and zero on a machine without a window. */
k_u64_t k_port_mm_base(void);
k_u64_t k_port_mm_limit(void);

/*
 * The first address above the window of the mappings: the page of the
 * restorer of a signal handler, the guard page and the stack of a program
 * live above it, and a mapping that reached it would take them. It is the
 * limit of the heap and of `mmap`; the port owns the number because the
 * layout of the top of the window is the port's.
 */
k_u64_t k_port_mm_top(void);

/* The address space of the task that runs now, zero for a task of the kernel
 * (which has no memory of a program). */
k_u64_t k_port_mm_current(void);

/*
 * The page behind `vaddr` of the space: a frame from the arena of the
 * machine, mapped with the protection asked for, and the address of the
 * kernel that holds its bytes - the port writes the page through it before
 * the program sees it. NULL when the space or the frame is not there.
 */
void *k_port_mm_page(k_u64_t space, k_u64_t vaddr, k_u32_t prot);

/* The page is there and read-only and the program wrote it: the same frame
 * becomes writable, which is the first write into a shared mapping. NULL when
 * the page is not there or the space is not. */
void *k_port_mm_writable(k_u64_t space, k_u64_t vaddr);

/* The page is there and read-only and the program wrote it, and the mapping
 * is private: this space gets a frame of its own with a copy of the bytes,
 * writable. The frame it had is released. */
void *k_port_mm_private(k_u64_t space, k_u64_t vaddr);

/* Is the page present in the space? */
bool k_port_mm_present(k_u64_t space, k_u64_t vaddr);

/* The address of the kernel that holds the bytes of a page that is present,
 * NULL when it is not: the walk of a page table and the addition of the offset
 * of the address in the page. The kernel reads the page of a shared mapping
 * through it when it writes the bytes back to the file. */
void *k_port_mm_phys(k_u64_t space, k_u64_t vaddr);

/* Unmap the range and give the frames back to the machine. */
void k_port_mm_unmap(k_u64_t space, k_u64_t vaddr, k_size_t len);

#endif /* MICRO_OS_KERNEL_PORT_MM_H */
