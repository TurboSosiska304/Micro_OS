/*
 * Micro OS — kernel log subsystem.
 *
 * Provides printf-like logging with levels plus a ring buffer of the most
 * recent messages, which later becomes the source of /proc/kmsg (dmesg).
 */
#ifndef MICRO_OS_KERNEL_LOG_H
#define MICRO_OS_KERNEL_LOG_H

#include "kernel/types.h"

typedef enum {
    K_LOG_LEVEL_DEBUG = 0,
    K_LOG_LEVEL_INFO,
    K_LOG_LEVEL_WARN,
    K_LOG_LEVEL_ERROR,
    K_LOG_LEVEL_OFF
} k_log_level_t;

/* Reset the log subsystem and set the current level. */
void klog_init(k_log_level_t level);

void klog_set_level(k_log_level_t level);
k_log_level_t klog_get_level(void);

/* Print a message (level filtered). */
#if defined(__GNUC__)
/*
 * The attribute uses the reserved name __printf__: the usual name is a
 * macro in the library of Micro OS, and a macro cannot be the name of a
 * format.
 */
void klog(k_log_level_t level, const char *fmt, ...) __attribute__((format(__printf__, 2, 3)));
#else
void klog(k_log_level_t level, const char *fmt, ...);
#endif

const char *klog_level_name(k_log_level_t level);

/* Number of messages currently kept in the ring buffer. */
k_size_t klog_line_count(void);

/*
 * Copy retained messages (oldest first) into a caller supplied buffer.
 * Returns the number of bytes written, excluding the terminating '\0'.
 */
k_size_t klog_dump(char *out, k_size_t out_size);

/* Drop all retained messages. */
void klog_clear(void);

/*
 * The capture: the messages are collected instead of being printed, so that
 * the log of the boot can be written to a file.
 */
void klog_capture_start(void);
void klog_capture_stop(void);
const char *klog_captured_text(void);
k_size_t klog_captured_size(void);

#define kdebug(...) klog(K_LOG_LEVEL_DEBUG, __VA_ARGS__)
#define kinfo(...) klog(K_LOG_LEVEL_INFO, __VA_ARGS__)
#define kwarn(...) klog(K_LOG_LEVEL_WARN, __VA_ARGS__)
#define kerror(...) klog(K_LOG_LEVEL_ERROR, __VA_ARGS__)

#endif /* MICRO_OS_KERNEL_LOG_H */
