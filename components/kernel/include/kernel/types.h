/*
 * Micro OS — kernel basic types.
 *
 * Portable layer: must not depend on ESP-IDF, newlib extensions or any
 * platform specific header. Only C11 standard types are allowed here.
 */
#ifndef MICRO_OS_KERNEL_TYPES_H
#define MICRO_OS_KERNEL_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t k_u8_t;
typedef uint16_t k_u16_t;
typedef uint32_t k_u32_t;
typedef uint64_t k_u64_t;
typedef int8_t k_i8_t;
typedef int16_t k_i16_t;
typedef int32_t k_i32_t;
typedef int64_t k_i64_t;

typedef size_t k_size_t;
typedef ptrdiff_t k_ssize_t;

#define K_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define K_MIN(a, b) ((a) < (b) ? (a) : (b))
#define K_MAX(a, b) ((a) > (b) ? (a) : (b))

#define K_UNUSED(x) ((void)(x))

#ifndef NULL
#define NULL ((void *)0)
#endif

#endif /* MICRO_OS_KERNEL_TYPES_H */
