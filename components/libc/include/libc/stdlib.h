/*
 * Micro OS — the memory of the C library.
 *
 * The memory of a program comes from the memory manager of the kernel
 * through the ABI of the system calls. The priority of the task decides
 * where the memory comes from: the system utilities get the internal RAM,
 * the applications get the PSRAM.
 */
#ifndef MICRO_OS_LIBC_STDLIB_H
#define MICRO_OS_LIBC_STDLIB_H

#include <stddef.h>

/* The memory of the program. */
void *microos_malloc(size_t size);
void *microos_realloc(void *ptr, size_t size);
void *microos_calloc(size_t count, size_t size);
void microos_free(void *ptr);

#define malloc microos_malloc
#define realloc microos_realloc
#define calloc microos_calloc
#define free microos_free

#endif /* MICRO_OS_LIBC_STDLIB_H */
