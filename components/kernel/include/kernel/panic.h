/*
 * Micro OS — panic handler.
 */
#ifndef MICRO_OS_KERNEL_PANIC_H
#define MICRO_OS_KERNEL_PANIC_H

#include "kernel/types.h"

K_NORETURN void kernel_panic(const char *file, int line, const char *fmt, ...)
    K_FORMAT(__printf__, 3, 4);

#define KERNEL_PANIC(...) kernel_panic(__FILE__, __LINE__, __VA_ARGS__)

#endif /* MICRO_OS_KERNEL_PANIC_H */
