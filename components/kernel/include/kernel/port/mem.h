/*
 * Micro OS — platform port: the memory.
 *
 * Two kinds of memory exist on the board and the kernel may ask for
 * either: the internal SRAM (fast, scarce) and the PSRAM (large, slower).
 * The port is the only place that knows which allocator stands behind
 * them; on the host they are the process heap and the host PSRAM pool.
 */
#ifndef MICRO_OS_KERNEL_PORT_MEM_H
#define MICRO_OS_KERNEL_PORT_MEM_H

#include "kernel/types.h"

/* Memory information. */
k_size_t k_port_heap_free(void);
k_size_t k_port_heap_total(void);
k_size_t k_port_psram_free(void);
k_size_t k_port_psram_total(void);

/*
 * The most memory one task may own on this machine: the ceiling a fresh task
 * starts at and the one KSYS_RLIMIT_MEMORY may never be raised above
 * (kernel/task.h has the limits themselves). It is a duty of the port because
 * it is a fact of the machine - the PSRAM of the board, the window of the
 * programs on x86-64 - and a kernel that named one number for all of them
 * would either promise a board more than it has or give a laptop less than it
 * can (ADR 0057).
 */
k_u64_t k_port_memory_limit(void);

/*
 * Allocate memory from the internal heap or from the PSRAM. The size 0
 * means "ask the system for a default block".
 */
void *k_port_alloc(k_size_t size, bool psram);
void k_port_free(void *ptr);

#endif /* MICRO_OS_KERNEL_PORT_MEM_H */
