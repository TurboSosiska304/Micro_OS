/*
 * Micro OS — compile time kernel configuration.
 *
 * Every macro can be overridden from the build system (-D...), so the same
 * sources are used for the ESP32 firmware and for the host/simulator build.
 */
#ifndef MICRO_OS_KERNEL_CONFIG_H
#define MICRO_OS_KERNEL_CONFIG_H

#include "kernel/log.h"

#ifndef KERNEL_NAME
#define KERNEL_NAME "MicroOS"
#endif

#define KERNEL_VERSION_MAJOR 0
#define KERNEL_VERSION_MINOR 4
#define KERNEL_VERSION_PATCH 0

/* A pre-release carries a suffix ("-rc1", "-pre") until the release drops it:
 * the tag is vX.Y.Z or vX.Y.Z-suffix, and the integrity check of a release
 * compares the three numbers with the tag. */
#define KERNEL_VERSION_SUFFIX "-rc2"

/* Size of the in-kernel log ring buffer (number of retained messages). */
#ifndef KERNEL_LOG_LINES
#define KERNEL_LOG_LINES 32
#endif

/* Maximum length of a single log line, including the trailing '\0'. */
#ifndef KERNEL_LOG_LINE_MAX
#define KERNEL_LOG_LINE_MAX 160
#endif

/* Default log level at boot: debug messages are compiled in but muted. */
#ifndef KERNEL_LOG_DEFAULT_LEVEL
#define KERNEL_LOG_DEFAULT_LEVEL K_LOG_LEVEL_INFO
#endif

/*
 * The kernel is written for a board without an MMU (the ESP32): the user
 * and the kernel share one address space, a task switch means switching
 * the stack pointer, and fork() is a concession that shares the memory of
 * the parent and re-runs its program.
 *
 * Everything that exists only because of that is kept behind this one
 * flag. A port for a processor with an MMU (the plan is a real ARM) sets
 * it to 0, and the same sources start offering what the flag takes away:
 * an address space of its own, a real fork, protection at the edge of the
 * system call. The flag is a property of the port, not a second kernel.
 */
#ifndef KERNEL_NOMMU
#define KERNEL_NOMMU 1
#endif

#if !KERNEL_NOMMU
#error "the port for a processor with an MMU is not written yet: see docs/roadmap.md"
#endif

/* Board identification (set by the build system). */
#ifndef KERNEL_BOARD_NAME
#define KERNEL_BOARD_NAME "unknown"
#endif

#ifndef KERNEL_BOARD_DESC
#define KERNEL_BOARD_DESC "unknown board"
#endif

#endif /* MICRO_OS_KERNEL_CONFIG_H */
