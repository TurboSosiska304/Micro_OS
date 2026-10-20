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
#define KERNEL_VERSION_MINOR 1
#define KERNEL_VERSION_PATCH 0

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

/* Board identification (set by the build system). */
#ifndef KERNEL_BOARD_NAME
#define KERNEL_BOARD_NAME "unknown"
#endif

#ifndef KERNEL_BOARD_DESC
#define KERNEL_BOARD_DESC "unknown board"
#endif

#endif /* MICRO_OS_KERNEL_CONFIG_H */
