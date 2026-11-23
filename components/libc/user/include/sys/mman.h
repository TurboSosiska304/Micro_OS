/*
 * Micro OS — the memory a program maps, in the shape of POSIX.
 *
 * The three bits of the protection, the three flags of a mapping and the page
 * of the calls are the numbers of the ABI (kernel/ksys.h); the library adds
 * the names a portable source uses. `MAP_FAILED` is the one address that is
 * not a mapping, as it is in POSIX.
 */
#ifndef MICRO_OS_ULIBC_SYS_MMAN_H
#define MICRO_OS_ULIBC_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#include "kernel/ksys.h"

#define PROT_NONE 0
#define PROT_READ KSYS_PROT_READ
#define PROT_WRITE KSYS_PROT_WRITE
#define PROT_EXEC KSYS_PROT_EXEC

#define MAP_SHARED KSYS_MAP_SHARED
#define MAP_PRIVATE KSYS_MAP_PRIVATE
#define MAP_ANONYMOUS KSYS_MAP_ANONYMOUS
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

/*
 * A range of the memory of the program, backed by a file or by zeros. The
 * pages are made when the program touches them, so a mapping of a file of
 * 64 MiB costs one page. `addr` of NULL asks for any free range; a non-NULL
 * one is a hint. The answer is the address of the range, or MAP_FAILED with
 * `errno` (ENOMEM, EINVAL, EBADF, ENOSYS on a machine without pages).
 */
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);

/* Give the range back. The shared pages go to their file first. */
int munmap(void *addr, size_t len);

/* Write the dirty pages of the shared mappings of the range back to their
 * files. A private mapping has nothing to write. */
int msync(void *addr, size_t len);

#endif /* MICRO_OS_ULIBC_SYS_MMAN_H */
