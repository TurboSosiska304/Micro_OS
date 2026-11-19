/*
 * Micro OS — what the system is, in words and numbers.
 *
 * The upper layers (the shell, /proc, the report of mfetch) want to know
 * how much memory is left and on what kind of machine they run. They must
 * not ask the platform for it: they ask the kernel, and the kernel asks
 * the port. This is the same direction as Linux, where the drivers use
 * the generic memory and time interfaces instead of the arch primitives.
 */
#ifndef MICRO_OS_KERNEL_SYSINFO_H
#define MICRO_OS_KERNEL_SYSINFO_H

#include "kernel/types.h"

/* The platform in short: "esp32", "host". */
const char *k_sysinfo_platform_name(void);

/* The architecture in words: "xtensa-esp32", "x86_64". */
const char *k_sysinfo_arch(void);

/* The internal memory of the chip: the fast one, the scarce one. */
k_size_t k_sysinfo_heap_total(void);
k_size_t k_sysinfo_heap_free(void);

/* The external memory (PSRAM), when the board has it. */
k_size_t k_sysinfo_psram_total(void);
k_size_t k_sysinfo_psram_free(void);

/* Microseconds since the boot, for the measurements of the shell. */
k_u64_t k_sysinfo_uptime_us(void);

#endif /* MICRO_OS_KERNEL_SYSINFO_H */
