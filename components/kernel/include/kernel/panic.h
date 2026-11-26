/*
 * Micro OS — panic handler.
 */
#ifndef MICRO_OS_KERNEL_PANIC_H
#define MICRO_OS_KERNEL_PANIC_H

#include "kernel/types.h"

#if defined(__GNUC__)
[[noreturn]] void kernel_panic(const char *file, int line, const char *fmt, ...)
    [[gnu::format(__printf__, 3, 4)]];
#else
[[noreturn]] void kernel_panic(const char *file, int line, const char *fmt, ...);
#endif

#define KERNEL_PANIC(...) kernel_panic(__FILE__, __LINE__, __VA_ARGS__)

#endif /* MICRO_OS_KERNEL_PANIC_H */
