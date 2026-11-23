/*
 * Micro OS — the memory a program maps.
 *
 * A program of this system has a heap and mappings of files, and neither is
 * memory until the program touches it: a region is a record of the address
 * space and the page behind an address is made, filled and mapped on the
 * fault. That is what makes `mmap` of a file of 64 MiB a call that costs one
 * page, and what makes `execve` of a program of 4 MiB a call that costs the
 * pages the program runs.
 *
 * The memory belongs to the address space of a program and not to the task:
 * the port names the space (CR3 on x86-64), a program that is being placed by
 * `execve` has no task yet, and the fault of a program is answered for the
 * space the processor is on. A machine without page tables has no space and
 * answers ENOSYS to every call of this file, as it does to `execve`.
 *
 * The layout of a space, from the base of the window of the port:
 *
 *   image (text, data, bss)   the regions `execve` makes, at the base
 *   heap                      from the end of the image up, grown by `brk`
 *   mappings                  anywhere above the heap, below the top
 *   stack, guard, restorer    above `k_port_mm_top()`, the port's own
 *
 * A mapping may not cross the heap or the top, and `brk` may not cross a
 * mapping: both answer ENOMEM with the break where it was, which is the
 * refusal of POSIX and not a panic of the kernel.
 */
#ifndef MICRO_OS_KERNEL_MM_H
#define MICRO_OS_KERNEL_MM_H

#include "kernel/exec.h"
#include "kernel/task.h"
#include "kernel/types.h"
#include "kernel/vfs.h"

/* The page of the machine, as the ABI names it: a mapping is a whole number
 * of pages and an offset into a file is a multiple of it. */
#define K_MM_PAGE_SIZE 4096u

/* The protection of the pages of a region: the three bits of the ABI. */
#define K_MM_PROT_READ 0x01u
#define K_MM_PROT_WRITE 0x02u
#define K_MM_PROT_EXEC 0x04u

/* The flags of a mapping: shared with the file, private to the program, or
 * not backed by a file at all. One of the first two is always set. */
#define K_MM_SHARED 0x01u
#define K_MM_PRIVATE 0x02u
#define K_MM_ANONYMOUS 0x04u

/* The marks the kernel puts on a region it made itself: the image of a
 * program and the heap of its `brk`. They are not flags of the ABI. */
#define K_MM_IMAGE 0x0100u
#define K_MM_HEAP 0x0200u

/* One region of an address space. The records live in the heap of the kernel
 * and the list is in the order of the addresses. */
struct k_mm_region {
    struct k_mm_region *next;
    k_u64_t start;  /* the first address, a page */
    k_u64_t len;    /* the length, a whole number of pages */
    k_u64_t offset; /* the offset in the file of the first page */
    k_u32_t prot;
    k_u32_t flags;
    bool file_open;
    char path[VFS_PATH_MAX]; /* the name the mapping was made with */
    struct vfs_file file;    /* the private open file of the mapping */
    _Alignas(8) k_u8_t priv[VFS_FILE_DATA_SIZE];
    k_u8_t *dirty; /* one bit per page, shared mappings only */
};

/* An address space: the handle of the port. Zero is "no space", the answer
 * of a machine without page tables and of a task of the kernel. */
typedef k_u64_t k_mm_space_t;

/* The space of the caller. */
k_mm_space_t k_mm_current(void);

/*
 * A new mapping of `len` bytes with the protection and the flags of the ABI.
 * `address` is a hint (NULL for "anywhere"); `path` names the file of the
 * mapping and is NULL for an anonymous one, and `offset` is a page-aligned
 * offset into it. The address the mapping got is written to `out`.
 */
int k_mm_map(k_mm_space_t space, k_u64_t address, k_size_t len, k_u32_t prot, k_u32_t flags,
             const char *path, k_u64_t offset, k_u64_t *out);

/* Give a range of mappings back: the shared pages go to their file first, the
 * frames go to the machine. A range that cuts a mapping in two is EINVAL. */
int k_mm_unmap(k_mm_space_t space, k_u64_t address, k_size_t len);

/* Write the dirty pages of the shared mappings of a range back to their
 * files. A private mapping has nothing to write. */
int k_mm_msync(k_mm_space_t space, k_u64_t address, k_size_t len);

/*
 * The heap. `address` of zero asks for the current break; a larger one grows
 * the heap and a smaller one gives the pages above the new end back. The
 * answer is 0 and the new break in `out`, or ENOMEM when the heap would cross
 * a mapping or the top of the window.
 */
int k_mm_brk(k_mm_space_t space, k_u64_t address, k_u64_t *out);

/* Move the beginning of the heap past the vectors of the entry of a program,
 * which the port writes below the stack. */
int k_mm_brk_base(k_mm_space_t space, k_u64_t address);

/*
 * The memory of an image: the text, the data and the zeroed part, taken from
 * the file on the fault. The call is the whole of what the port needs of this
 * file for `execve`: it records the regions and sets the heap after them.
 */
int k_mm_image(k_mm_space_t space, const struct k_exec_image *image);

/* The fault of a page: 0 when the instruction may be run again, -1 when the
 * address is not memory the program asked for. */
int k_mm_fault(k_mm_space_t space, k_u64_t address, bool write);

/*
 * Make a range present and filled, for the kernel that is about to read or to
 * write the memory of a program: a copy of a system call, the vectors of the
 * entry of a program. A page that is already there is left alone; a page of a
 * mapping that is not there yet is made and filled from its file (or with
 * zeros), and `write` says whether the access of the caller is a write, which
 * is what a private page needs to know to be mapped writable at once.
 *
 * A range that runs out of the mappings of the program is KERR_FAULT and the
 * pages before it are made; the caller still checks the range against the
 * tables of the port, because a page of the stack or of the text is present
 * without being a mapping.
 */
int k_mm_touch(k_mm_space_t space, k_u64_t address, k_size_t len, bool write);

/*
 * The child of a fork gets the memory of its parent: the same regions, in the
 * same places, with a file of their own each. The pages themselves are the
 * work of the port - it copies the tables, shares the frames and marks them
 * copy-on-write - so a page the child has never touched is still a page of
 * the file, and the fault of the child fills it from the file as the fault of
 * the parent did.
 */
int k_mm_fork(k_mm_space_t parent, k_mm_space_t child);

/* The program is gone: the mappings, the files and the records go. */
void k_mm_release(k_mm_space_t space);

#endif /* MICRO_OS_KERNEL_MM_H */
